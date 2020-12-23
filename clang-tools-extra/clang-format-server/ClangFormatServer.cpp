#include <zmq.hpp>

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Format/Format.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Process.h"

using namespace llvm;
using clang::tooling::Replacements;

namespace clang {
namespace format {

static FileID createInMemoryFile(StringRef FileName, MemoryBuffer *Source,
                                 SourceManager &Sources, FileManager &Files,
                                 llvm::vfs::InMemoryFileSystem *MemFS) {
  MemFS->addFileNoOwn(FileName, 0, Source->getMemBufferRef());
  auto File = Files.getOptionalFileRef(FileName);
  assert(File && "File not added to MemFS?");
  return Sources.createFileID(*File, SourceLocation(), SrcMgr::C_User);
}

static bool fillRanges(MemoryBuffer *Code,
                       std::vector<tooling::Range> &Ranges) {
  Ranges.push_back(tooling::Range(0, Code->getBufferSize()));
  return false;
}

// Returns true on error.
static bool format(StringRef Content, std::string &FormattedCode) {
  // On Windows, overwriting a file with an open file mapping doesn't work,
  // so read the whole file into memory when formatting in-place.
  std::unique_ptr<llvm::MemoryBuffer> Code =
      llvm::MemoryBuffer::getMemBufferCopy(Content);
  if (Code->getBufferSize() == 0)
    return false; // Empty files are formatted correctly.

  StringRef BufStr = Code->getBuffer();
  const char *InvalidBOM = SrcMgr::ContentCache::getInvalidBOM(BufStr);

  if (InvalidBOM) {
    errs() << "error: encoding with unsupported byte order mark \""
           << InvalidBOM << "\" detected";
    return true;
  }

  std::vector<tooling::Range> Ranges;
  if (fillRanges(Code.get(), Ranges))
    return true;

  llvm::Expected<FormatStyle> FormatStyle =
      clang::format::getGoogleStyle(FormatStyle::LK_Cpp);
  if (!FormatStyle) {
    llvm::errs() << llvm::toString(FormatStyle.takeError()) << "\n";
    return true;
  }

  FormatStyle->SortIncludes = true;
  unsigned CursorPosition = 0;
  Replacements Replaces = sortIncludes(*FormatStyle, Code->getBuffer(), Ranges,
                                       "", &CursorPosition);
  auto ChangedCode = tooling::applyAllReplacements(Code->getBuffer(), Replaces);
  if (!ChangedCode) {
    llvm::errs() << llvm::toString(ChangedCode.takeError()) << "\n";
    return true;
  }

  // Get new affected ranges after sorting `#includes`.
  Ranges = tooling::calculateRangesAfterReplacements(Replaces, Ranges);
  FormattingAttemptStatus Status;

  StringRef FileName = "temp.cc";
  Replacements FormatChanges =
      reformat(*FormatStyle, *ChangedCode, Ranges, FileName, &Status);

  if (!Status.FormatComplete) {
    return true;
  }

  Replaces = Replaces.merge(FormatChanges);

  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem(
      new llvm::vfs::InMemoryFileSystem);
  FileManager Files(FileSystemOptions(), InMemoryFileSystem);
  DiagnosticsEngine Diagnostics(
      IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs),
      new DiagnosticOptions);
  SourceManager Sources(Diagnostics, Files);
  FileID ID = createInMemoryFile(FileName, Code.get(), Sources, Files,
                                 InMemoryFileSystem.get());
  Rewriter Rewrite(Sources, LangOptions());
  tooling::applyAllReplacements(Replaces, Rewrite);

  raw_string_ostream StrStream(FormattedCode);
  Rewrite.getEditBuffer(ID).write(StrStream);

  return false;
}

} // namespace format
} // namespace clang

int main(int argc, const char **argv) {
  llvm::InitLLVM X(argc, argv);

  zmq::context_t Context(1);
  zmq::socket_t Socket(Context, ZMQ_REP);
  Socket.bind("tcp://*:5001");

  outs() << "---------------------------------\n";
  outs() << "Server Started. Listening at 3001\n";
  outs() << "---------------------------------\n";

  while (true) {
    zmq::message_t Request;
    Socket.recv(Request);

    std::string FormattedCode;
    bool Error = clang::format::format(Request.to_string(), FormattedCode);
    if (Error) {
      FormattedCode = Request.to_string();
    }

    Socket.send(zmq::buffer(FormattedCode));
  }

  return 0;
}
