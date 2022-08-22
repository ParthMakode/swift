//===-- LocalizationFormat.cpp - Format for Diagnostic Messages -*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the format for localized diagnostic messages.
//
//===----------------------------------------------------------------------===//

#include "swift/Localization/LocalizationFormat.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include <cstdint>
#include <string>
#include <system_error>
#include <type_traits>

namespace {

enum LocalDiagID : uint32_t {
#define DIAG(KIND, ID, Options, Text, Signature) ID,
#include "swift/AST/DiagnosticsAll.def"
  NumDiags
};

static constexpr const char *const diagnosticNameStrings[] = {
#define DIAG(KIND, ID, Options, Text, Signature) " [" #ID "]",
#include "swift/AST/DiagnosticsAll.def"
    "<not a diagnostic>",
};

} // namespace

namespace llvm {
namespace yaml {

template <> struct ScalarEnumerationTraits<LocalDiagID> {
  static void enumeration(IO &io, LocalDiagID &value) {
#define DIAG(KIND, ID, Options, Text, Signature)                               \
  io.enumCase(value, #ID, LocalDiagID::ID);
#include "swift/AST/DiagnosticsAll.def"
    // Ignore diagnostic IDs that are available in the YAML file and not
    // available in the `.def` file.
    if (io.matchEnumFallback())
      value = LocalDiagID::NumDiags;
  }
};

} // namespace yaml
} // namespace llvm

namespace swift {
namespace diag {

void SerializedLocalizationWriter::insert(swift::DiagID id,
                                          llvm::StringRef translation) {
  generator.insert(static_cast<uint32_t>(id), translation);
}

bool SerializedLocalizationWriter::emit(llvm::StringRef filePath) {
  assert(llvm::sys::path::extension(filePath) == ".db");
  std::error_code error;
  llvm::raw_fd_ostream OS(filePath, error, llvm::sys::fs::OF_None);
  if (OS.has_error()) {
    return true;
  }

  offset_type offset;
  {
    llvm::support::endian::write<offset_type>(OS, 0, llvm::support::little);
    offset = generator.Emit(OS);
  }
  OS.seek(0);
  llvm::support::endian::write(OS, offset, llvm::support::little);
  OS.close();

  return OS.has_error();
}

void LocalizationProducer::initializeIfNeeded() {
  if (state != NotInitialized)
    return;

  if (initializeImpl())
    state = Initialized;
  else
    state = FailedInitialization;
}

llvm::StringRef
LocalizationProducer::getMessageOr(swift::DiagID id,
                                   llvm::StringRef defaultMessage) {
  initializeIfNeeded();
  if (getState() == FailedInitialization) {
    return defaultMessage;
  }

  auto localizedMessage = getMessage(id);
  if (localizedMessage.empty())
    return defaultMessage;
  if (printDiagnosticNames) {
    llvm::StringRef diagnosticName(diagnosticNameStrings[(unsigned)id]);
    auto localizedDebugDiagnosticMessage =
        localizationSaver.save(localizedMessage.str() + diagnosticName.str());
    return localizedDebugDiagnosticMessage;
  }
  return localizedMessage;
}

LocalizationProducerState LocalizationProducer::getState() const {
  return state;
}

SerializedLocalizationProducer::SerializedLocalizationProducer(
    std::unique_ptr<llvm::MemoryBuffer> buffer, bool printDiagnosticNames)
    : LocalizationProducer(printDiagnosticNames), Buffer(std::move(buffer)) {
}

bool SerializedLocalizationProducer::initializeImpl() {
  auto base =
      reinterpret_cast<const unsigned char *>(Buffer.get()->getBufferStart());
  auto tableOffset = endian::read<offset_type>(base, little);
  SerializedTable.reset(SerializedLocalizationTable::Create(
      base + tableOffset, base + sizeof(offset_type), base));
  return true;
}

llvm::StringRef
SerializedLocalizationProducer::getMessage(swift::DiagID id) const {
  auto value = SerializedTable.get()->find(id);
  if (value.getDataLen() == 0)
    return llvm::StringRef();
  return {(const char *)value.getDataPtr(), value.getDataLen()};
}

YAMLLocalizationProducer::YAMLLocalizationProducer(llvm::StringRef filePath,
                                                   bool printDiagnosticNames)
    : LocalizationProducer(printDiagnosticNames), filePath(filePath) {
}

bool YAMLLocalizationProducer::initializeImpl() {
  auto FileBufOrErr = llvm::MemoryBuffer::getFileOrSTDIN(filePath);
  llvm::MemoryBuffer *document = FileBufOrErr->get();
  diag::LocalizationInput yin(document->getBuffer());
  yin >> diagnostics;
  unknownIDs = std::move(yin.unknownIDs);
  return true;
}

llvm::StringRef YAMLLocalizationProducer::getMessage(swift::DiagID id) const {
  return diagnostics[(unsigned)id];
}

void YAMLLocalizationProducer::forEachAvailable(
    llvm::function_ref<void(swift::DiagID, llvm::StringRef)> callback) {
  initializeIfNeeded();
  if (getState() == FailedInitialization) {
    return;
  }

  for (uint32_t i = 0, n = diagnostics.size(); i != n; ++i) {
    auto translation = diagnostics[i];
    if (!translation.empty())
      callback(static_cast<swift::DiagID>(i), translation);
  }
}

std::unique_ptr<LocalizationProducer>
LocalizationProducer::producerFor(llvm::StringRef locale, llvm::StringRef path,
                                  bool printDiagnosticNames) {
  llvm::SmallString<128> filePath(path);
  llvm::sys::path::append(filePath, locale);
  llvm::sys::path::replace_extension(filePath, ".db");

  // If the serialized diagnostics file not available,
  // fallback to the `YAML` file.
  if (llvm::sys::fs::exists(filePath)) {
    if (auto file = llvm::MemoryBuffer::getFile(filePath)) {
      return std::make_unique<diag::SerializedLocalizationProducer>(
          std::move(file.get()), printDiagnosticNames);
    }
  } else {
    llvm::sys::path::replace_extension(filePath, ".yaml");
    // In case of missing localization files, we should fallback to messages
    // from `.def` files.
    if (llvm::sys::fs::exists(filePath)) {
      return std::make_unique<diag::YAMLLocalizationProducer>(
          filePath.str(), printDiagnosticNames);
    }

    llvm::sys::path::replace_extension(filePath, ".strings");
    if (llvm::sys::fs::exists(filePath)) {
      return std::make_unique<diag::StringsLocalizationProducer>(
          filePath.str(), printDiagnosticNames);
    }
  }

  return std::unique_ptr<LocalizationProducer>();
}

llvm::Optional<uint32_t> LocalizationInput::readID(llvm::yaml::IO &io) {
  LocalDiagID diagID;
  io.mapRequired("id", diagID);
  if (diagID == LocalDiagID::NumDiags)
    return llvm::None;
  return static_cast<uint32_t>(diagID);
}

template <typename T, typename Context>
typename std::enable_if<llvm::yaml::has_SequenceTraits<T>::value, void>::type
readYAML(llvm::yaml::IO &io, T &Seq, T &unknownIDs, bool, Context &Ctx) {
  unsigned count = io.beginSequence();
  if (count) {
    Seq.resize(LocalDiagID::NumDiags);
  }

  for (unsigned i = 0; i < count; ++i) {
    void *SaveInfo;
    if (io.preflightElement(i, SaveInfo)) {
      io.beginMapping();

      // If the current diagnostic ID is available in YAML and in `.def`, add it
      // to the diagnostics array. Otherwise, re-parse the current diagnostic
      // id as a string and store it in `unknownIDs` array.
      if (auto id = LocalizationInput::readID(io)) {
        // YAML file isn't guaranteed to have diagnostics in order of their
        // declaration in `.def` files, to accommodate that we need to leave
        // holes in diagnostic array for diagnostics which haven't yet been
        // localized and for the ones that have `id` indicates their position.
        io.mapRequired("msg", Seq[*id]);
      } else {
        std::string unknownID, message;
        // Read "raw" id since it doesn't exist in `.def` file.
        io.mapRequired("id", unknownID);
        io.mapRequired("msg", message);
        unknownIDs.push_back(unknownID);
      }
      io.endMapping();
      io.postflightElement(SaveInfo);
    }
  }
  io.endSequence();
}

template <typename T>
typename std::enable_if<llvm::yaml::has_SequenceTraits<T>::value,
                        LocalizationInput &>::type
operator>>(LocalizationInput &yin, T &diagnostics) {
  llvm::yaml::EmptyContext Ctx;
  if (yin.setCurrentDocument()) {
    // If YAML file's format doesn't match the current format in
    // DiagnosticMessageFormat, will throw an error.
    readYAML(yin, diagnostics, yin.unknownIDs, true, Ctx);
  }
  return yin;
}

void DefToYAMLConverter::convert(llvm::raw_ostream &out) {
  for (auto i : swift::indices(IDs)) {
    out << "- id: " << IDs[i] << "\n";

    const std::string &msg = Messages[i];

    out << "  msg: \"";
    // Add an escape character before a double quote `"` or a backslash `\`.
    for (unsigned j = 0; j < msg.length(); ++j) {
      if (msg[j] == '"') {
        out << '\\';
        out << '"';
      } else if (msg[j] == '\\') {
        out << '\\';
        out << '\\';
      } else {
        out << msg[j];
      }
    }
    out << "\"\r\n";
  }
}

void DefToStringsConverter::convert(llvm::raw_ostream &out) {
  // "<id>" = "<msg>";
  for (auto i : swift::indices(IDs)) {
    out << "\"" << IDs[i] << "\"";
    out << " = ";

    const std::string &msg = Messages[i];

    out << "\"";
    for (unsigned j = 0; j < msg.length(); ++j) {
      // Escape '"' found in the message.
      if (msg[j] == '"')
        out << '\\';

      out << msg[j];
    }

    out << "\";\r\n";
  }
}

bool StringsLocalizationProducer::initializeImpl() {
  auto FileBufOrErr = llvm::MemoryBuffer::getFileOrSTDIN(filePath);
  llvm::MemoryBuffer *document = FileBufOrErr->get();
  readStringsFile(document, diagnostics);
  return true;
}

llvm::StringRef
StringsLocalizationProducer::getMessage(swift::DiagID id) const {
  return diagnostics[(unsigned)id];
}

void StringsLocalizationProducer::forEachAvailable(
    llvm::function_ref<void(swift::DiagID, llvm::StringRef)> callback) {
  initializeIfNeeded();
  if (getState() == FailedInitialization) {
    return;
  }

  for (uint32_t i = 0, n = diagnostics.size(); i != n; ++i) {
    auto translation = diagnostics[i];
    if (!translation.empty())
      callback(static_cast<swift::DiagID>(i), translation);
  }
}

void StringsLocalizationProducer::readStringsFile(
    llvm::MemoryBuffer *in, std::vector<std::string> &diagnostics) {
  std::map<std::string, unsigned> diagLocs;
#define DIAG(KIND, ID, Options, Text, Signature)                               \
  diagLocs[#ID] = static_cast<unsigned>(LocalDiagID::ID);
#include "swift/AST/DiagnosticsAll.def"
#undef DIAG

  // Allocate enough slots to fit all the possible diagnostics
  // this helps to identify which diagnostics are missing.
  diagnostics.resize(LocalDiagID::NumDiags);

  // The format is as follows:
  //
  // - comment: /* ... */
  // - translation: "<id>" = "<message>";
  auto buffer = in->getBuffer();
  while (!buffer.empty()) {
    // consume comment.
    if (buffer.startswith("/*")) {
      auto endOfComment = buffer.find("*/");
      assert(endOfComment != std::string::npos);
      // Consume the comment and trailing `*/`
      buffer = buffer.drop_front(endOfComment + 2).ltrim();
      continue;
    }

    assert(buffer.startswith("\"") && "malformed diagnostics file");

    // Consume leading `"`
    buffer = buffer.drop_front();

    // Valid diagnostic id cannot have any `"` in it.
    auto idSize = buffer.find_first_of('\"');
    assert(idSize != std::string::npos);

    std::string id(buffer.data(), idSize);

    // consume id and `" = "`. There could be a variable number of
    // spaces on each side of `=`.
    {
      // Consume id, trailing `"`, and all spaces before `=`
      buffer = buffer.drop_front(idSize + 1).ltrim(' ');

      // Consume `=` and all trailing spaces until `"`
      {
        assert(!buffer.empty() && buffer.front() == '=');
        buffer = buffer.drop_front().ltrim(' ');
      }

      // Consume `"` at the beginning of the diagnostic message.
      {
        assert(!buffer.empty() && buffer.front() == '\"');
        buffer = buffer.drop_front();
      }
    }

    llvm::SmallString<64> msg;
    {
      bool isValid = false;
      // Look for `";` which denotes the end of message
      for (unsigned i = 0, n = buffer.size(); i != n; ++i) {
        if (buffer[i] != '\"') {
          msg.push_back(buffer[i]);
          continue;
        }

        // Leading `"` has been comsumed.
        assert(i > 0);

        // Let's check whether this `"` is escaped, and if so - continue
        // because `"` is part of the message.
        if (buffer[i - 1] == '\\') {
          // Drop `\` added for escaping.
          msg.pop_back();
          msg.push_back(buffer[i]);
          continue;
        }

        // If current `"` was not escaped and it's followed by `;` -
        // we have reached the end of the message, otherwise
        // the input is malformed.
        if (i + 1 < n && buffer[i + 1] == ';') {
          // Consume the message and its trailing info.
          buffer = buffer.drop_front(i + 2).ltrim();
          // Mark message as valid.
          isValid = true;
          break;
        } else {
          llvm_unreachable("malformed diagnostics file");
        }
      }

      assert(isValid && "malformed diagnostic message");
    }

    // Check whether extracted diagnostic still exists in the
    // system and if not - record as unknown.
    {
      auto existing = diagLocs.find(id);
      if (existing != diagLocs.end()) {
        diagnostics[existing->second] = std::string(msg);
      } else {
        llvm::errs() << "[!] Unknown diagnostic: " << id << '\n';
      }
    }
  }
}

} // namespace diag
} // namespace swift
