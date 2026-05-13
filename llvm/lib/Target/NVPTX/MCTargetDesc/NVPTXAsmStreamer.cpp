//===-- NVPTXAsmStreamer.cpp - NVPTX assembly text output ------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "NVPTXAsmStreamer.h"
#include "NVPTXUtilities.h"
#include "cl_common_defines.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"

using namespace llvm;

static inline char toOctal(int X) { return (X & 7) + '0'; }

NVPTXAsmStreamer::NVPTXAsmStreamer(MCContext &Context,
                                   std::unique_ptr<formatted_raw_ostream> os,
                                   std::unique_ptr<MCInstPrinter> printer,
                                   std::unique_ptr<MCCodeEmitter> emitter,
                                   std::unique_ptr<MCAsmBackend> asmbackend)
    : MCAsmBaseStreamer(Context), OSOwner(std::move(os)), OS(*OSOwner),
      MAI(Context.getAsmInfo()), InstPrinter(std::move(printer)),
      Assembler(std::make_unique<MCAssembler>(
          Context, std::move(asmbackend), std::move(emitter),
          (asmbackend) ? asmbackend->createObjectWriter(NullStream) : nullptr)),
      CommentStream(CommentToEmit) {
  assert(InstPrinter);
  if (Assembler->getBackendPtr())
    setAllowAutoPadding(Assembler->getBackend().allowAutoPadding());

  Context.setUseNamesOnTempLabels(true);

  auto *TO = Context.getTargetOptions();
  IsVerboseAsm = TO->AsmVerbose;
  if (IsVerboseAsm)
    InstPrinter->setCommentStream(CommentStream);
  ShowInst = TO->ShowMCInst;
  switch (TO->MCUseDwarfDirectory) {
  case MCTargetOptions::DisableDwarfDirectory:
    UseDwarfDirectory = false;
    break;
  case MCTargetOptions::EnableDwarfDirectory:
    UseDwarfDirectory = true;
    break;
  case MCTargetOptions::DefaultDwarfDirectory:
    UseDwarfDirectory = Context.getAsmInfo()->enableDwarfFileDirectoryDefault();
    break;
  }
}

void NVPTXAsmStreamer::AddComment(const Twine &T, bool EOL) {
  if (!IsVerboseAsm)
    return;

  T.toVector(CommentToEmit);

  if (EOL)
    CommentToEmit.push_back('\n'); // Place comment in a new line.
}

void NVPTXAsmStreamer::EmitCommentsAndEOL() {
  if (CommentToEmit.empty() && CommentStream.GetNumBytesInBuffer() == 0) {
    OS << '\n';
    return;
  }

  StringRef Comments = CommentToEmit;

  assert(Comments.back() == '\n' && "Comment array not newline terminated");
  do {
    // Emit a line of comments.
    OS.PadToColumn(MAI->getCommentColumn());
    size_t Position = Comments.find('\n');
    OS << MAI->getCommentString() << ' ' << Comments.substr(0, Position)
       << '\n';

    Comments = Comments.substr(Position + 1);
  } while (!Comments.empty());

  CommentToEmit.clear();
}

void NVPTXAsmStreamer::emitRawComment(const Twine &T, bool TabPrefix) {
  if (TabPrefix)
    OS << '\t';
  OS << MAI->getCommentString() << T;
  EmitEOL();
}

void NVPTXAsmStreamer::emitExplicitComments() {
  StringRef Comments = ExplicitCommentToEmit;
  if (!Comments.empty())
    OS << Comments;
  ExplicitCommentToEmit.clear();
}

void NVPTXAsmStreamer::switchSection(MCSection *Section, uint32_t Subsection) {
  MCSectionSubPair Cur = getCurrentSection();
  if (!EmittedSectionDirective ||
      MCSectionSubPair(Section, Subsection) != Cur) {
    EmittedSectionDirective = true;
    MCTargetStreamer *TS = getTargetStreamer();
    TS->changeSection(Cur.first, Section, Subsection, OS);
  }
  MCStreamer::switchSection(Section, Subsection);
}

void NVPTXAsmStreamer::emitIntValue(uint64_t Value, unsigned Size,
                                    bool EmitDelimit) {
  emitValue(MCConstantExpr::create(Value, getContext()), Size, SMLoc(),
            EmitDelimit);
}

// Helper to check if given section corresponds to a DebugInfo section.
static bool isDebugSection(const MCSection *Sec) {
  StringRef Name = Sec->getName();
  return Name.starts_with(".debug_") || Name.starts_with(".zdebug_") ||
         Name.contains("debug");
}

void NVPTXAsmStreamer::emitValueImpl(const MCExpr *Value, unsigned Size,
                                     SMLoc Loc, bool EmitDelimit) {
  assert(Size <= 8 && "Invalid size");
  MCSection *CurrSec = getCurrentSectionOnly();
  assert(CurrSec && "Cannot emit contents before setting section!");

  // We don't need a directive to emit values in PTX. This is retained only for
  // debug_info sections.
  if (isDebugSection(CurrSec)) {
    const char *Directive = nullptr;
    switch (Size) {
    default:
      break;
    case 1:
      Directive = MAI->getData8bitsDirective();
      break;
    case 4:
      Directive = MAI->getData32bitsDirective();
      break;
    case 8:
      Directive = MAI->getData64bitsDirective();
      break;
    }

    if (!Directive) {
      assert(Size == 2 && "Expected 16-bit values here.");

      int64_t IntValue;
      if (!Value->evaluateAsAbsolute(IntValue))
        report_fatal_error("Don't know how to emit this value.");

      // Break down Int16 value into two Int8 values and emit them. TODO:
      // Current use-case is only for debug_info sections, investigate if this
      // needs to be uplifted outside isDebugSection check.
      for (unsigned I = 0; I < 2; I++) {
        uint64_t Int8ValueToEmit = IntValue >> (I * 8);
        Int8ValueToEmit &= ~0ULL >> 56;
        emitInt8(Int8ValueToEmit);
      }
      return;
    }

    OS << Directive;
  }

  if (Value->getKind() == MCExpr::SymbolRef) {
    const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*Value);
    StringRef SymName = SRE.getSymbol().getName();
    if (!SymName.starts_with(".debug")) {
      emitRawText(NVPTX::getValidPTXIdentifier(SymName));
      return;
    }
    // Fall through to the normal printing.
  }
  // Otherwise, print the Value normally.
  getContext().getAsmInfo()->printExpr(OS, *Value);
  if (isDebugSection(CurrSec)) {
    EmitEOL();
    return;
  }

  if (EmitDelimit)
    OS << ", ";
}

void NVPTXAsmStreamer::emitBytes(StringRef Data) {
  MCTargetStreamer *TS = getTargetStreamer();
  TS->emitRawBytes(Data);
}

void NVPTXAsmStreamer::emitLabel(MCSymbol *Symbol, SMLoc Loc) {
  MCStreamer::emitLabel(Symbol, Loc);
  Symbol->setOffset(0);
  Symbol->print(OS, MAI);
  OS << MAI->getLabelSuffix();

  EmitEOL();
}

bool NVPTXAsmStreamer::emitSymbolAttribute(MCSymbol *Symbol,
                                           MCSymbolAttr Attribute) {
  // Support only required symbols for PTX.
  switch (Attribute) {
  case MCSA_Global: // .globl/.global
    OS << MAI->getGlobalDirective();
    break;
  case MCSA_Weak:
    OS << MAI->getWeakDirective();
    break;
  case MCSA_Cold:
    return false;
  default:
    llvm_unreachable("Unsupported symbol attribute in NVPTXAsmStreamer.");
  }

  Symbol->print(OS, MAI);
  EmitEOL();

  return true;
}

void NVPTXAsmStreamer::emitInstruction(const MCInst &Inst,
                                       const MCSubtargetInfo &STI) {
  // Show the MCInst if enabled.
  if (ShowInst) {
    Inst.dump_pretty(getCommentOS(), InstPrinter.get(), "\n ", &getContext());
    getCommentOS() << "\n";
  }

  InstPrinter->printInst(&Inst, 0, "", STI, OS);
  EmitEOL();
}

void NVPTXAsmStreamer::emitRelocDirective(const MCExpr &Offset, StringRef Name,
                                          const MCExpr *Expr, SMLoc) {
  OS << "\t.reloc ";
  MAI->printExpr(OS, Offset);
  OS << ", " << Name;
  if (Expr) {
    OS << ", ";
    MAI->printExpr(OS, *Expr);
  }
  EmitEOL();
}

void NVPTXAsmStreamer::emitRawTextImpl(StringRef String) {
  String.consume_back("\n");
  OS << String;
  EmitEOL();
}

// Implementation of support for .loc and .file directives is duplicated from
// MCAsmStreamer.

void NVPTXAsmStreamer::PrintQuotedString(StringRef Data,
                                         raw_ostream &OS) const {
  OS << '"';

  for (unsigned char C : Data) {
    if (C == '"' || C == '\\') {
      OS << '\\' << (char)C;
      continue;
    }

    if (isPrint(C)) {
      OS << (char)C;
      continue;
    }

    switch (C) {
    case '\b':
      OS << "\\b";
      break;
    case '\f':
      OS << "\\f";
      break;
    case '\n':
      OS << "\\n";
      break;
    case '\r':
      OS << "\\r";
      break;
    case '\t':
      OS << "\\t";
      break;
    default:
      OS << '\\';
      OS << toOctal(C >> 6);
      OS << toOctal(C >> 3);
      OS << toOctal(C >> 0);
      break;
    }
  }

  OS << '"';
}

void NVPTXAsmStreamer::printDwarfFileDirective(
    unsigned FileNo, StringRef Directory, StringRef Filename,
    std::optional<MD5::MD5Result> Checksum, std::optional<StringRef> Source,
    bool UseDwarfDirectory, raw_svector_ostream &OS) const {
  SmallString<128> FullPathName;

  if (!UseDwarfDirectory && !Directory.empty()) {
    if (sys::path::is_absolute(Filename))
      Directory = "";
    else {
      FullPathName = Directory;
      sys::path::append(FullPathName, Filename);
      Directory = "";
      Filename = FullPathName;
    }
  }

  OS << "\t.file\t" << FileNo << ' ';
  if (!Directory.empty()) {
    PrintQuotedString(Directory, OS);
    OS << ' ';
  }
  PrintQuotedString(Filename, OS);
  if (Checksum)
    OS << " md5 0x" << Checksum->digest();
  if (Source) {
    OS << " source ";
    PrintQuotedString(*Source, OS);
  }
}

Expected<unsigned> NVPTXAsmStreamer::tryEmitDwarfFileDirective(
    unsigned FileNo, StringRef Directory, StringRef Filename,
    std::optional<MD5::MD5Result> Checksum, std::optional<StringRef> Source,
    unsigned CUID) {
  assert(CUID == 0 && "multiple CUs not supported by MCAsmStreamer");

  MCDwarfLineTable &Table = getContext().getMCDwarfLineTable(CUID);
  unsigned NumFiles = Table.getMCDwarfFiles().size();
  Expected<unsigned> FileNoOrErr =
      Table.tryGetFile(Directory, Filename, Checksum, Source,
                       getContext().getDwarfVersion(), FileNo);
  if (!FileNoOrErr)
    return FileNoOrErr.takeError();
  FileNo = FileNoOrErr.get();

  // Return early if this file is already emitted before or if target doesn't
  // support .file directive.
  if (NumFiles == Table.getMCDwarfFiles().size() || MAI->isAIX())
    return FileNo;

  SmallString<128> Str;
  raw_svector_ostream OS1(Str);
  printDwarfFileDirective(FileNo, Directory, Filename, Checksum, Source,
                          UseDwarfDirectory, OS1);

  MCTargetStreamer *TS = getTargetStreamer();
  assert(TS && "Expected target streamer for NVPTX backend.");
  TS->emitDwarfFileDirective(OS1.str());

  return FileNo;
}

void NVPTXAsmStreamer::emitDwarfFile0Directive(
    StringRef Directory, StringRef Filename,
    std::optional<MD5::MD5Result> Checksum, std::optional<StringRef> Source,
    unsigned CUID) {
  assert(CUID == 0);
  // .file 0 is new for DWARF v5.
  if (getContext().getDwarfVersion() < 5)
    return;
  // Inform MCDwarf about the root file.
  getContext().setMCLineTableRootFile(CUID, Directory, Filename, Checksum,
                                      Source);

  assert(!MAI->isAIX() && "Unexpected AIX in NVPTX asm streamer.");

  SmallString<128> Str;
  raw_svector_ostream OS1(Str);
  printDwarfFileDirective(0, Directory, Filename, Checksum, Source,
                          UseDwarfDirectory, OS1);

  MCTargetStreamer *TS = getTargetStreamer();
  assert(TS && "Expected target streamer for NVPTX backend.");
  TS->emitDwarfFileDirective(OS1.str());
}

/// Helper to emit common .loc directive flags, isa, and discriminator.
void NVPTXAsmStreamer::emitDwarfLocDirectiveFlags(unsigned Flags, unsigned Isa,
                                                  unsigned Discriminator) {
  if (!MAI->supportsExtendedDwarfLocDirective())
    return;

  if (Flags & DWARF2_FLAG_BASIC_BLOCK)
    OS << " basic_block";
  if (Flags & DWARF2_FLAG_PROLOGUE_END)
    OS << " prologue_end";
  if (Flags & DWARF2_FLAG_EPILOGUE_BEGIN)
    OS << " epilogue_begin";

  const unsigned OldFlags = getContext().getCurrentDwarfLoc().getFlags();
  if ((Flags & DWARF2_FLAG_IS_STMT) != (OldFlags & DWARF2_FLAG_IS_STMT)) {
    OS << " is_stmt ";
    OS << ((Flags & DWARF2_FLAG_IS_STMT) ? "1" : "0");
  }

  if (Isa)
    OS << " isa " << Isa;
  if (Discriminator)
    OS << " discriminator " << Discriminator;
}

/// Helper to emit the common suffix of .loc directives.
void NVPTXAsmStreamer::emitDwarfLocDirectiveSuffix(
    unsigned FileNo, unsigned Line, unsigned Column, unsigned Flags,
    unsigned Isa, unsigned Discriminator, StringRef FileName,
    StringRef Comment) {
  // Emit flags, isa, and discriminator.
  emitDwarfLocDirectiveFlags(Flags, Isa, Discriminator);

  // Emit verbose comment if enabled.
  if (IsVerboseAsm) {
    OS.PadToColumn(MAI->getCommentColumn());
    OS << MAI->getCommentString() << ' ';
    if (Comment.empty())
      OS << FileName << ':' << Line << ':' << Column;
    else
      OS << Comment;
  }

  // Emit end of line and update the baseclass state.
  EmitEOL();
  MCStreamer::emitDwarfLocDirective(FileNo, Line, Column, Flags, Isa,
                                    Discriminator, FileName, Comment);
}

void NVPTXAsmStreamer::emitDwarfLocDirective(unsigned FileNo, unsigned Line,
                                             unsigned Column, unsigned Flags,
                                             unsigned Isa,
                                             unsigned Discriminator,
                                             StringRef FileName,
                                             StringRef Comment) {
  // Emit the basic .loc directive.
  OS << "\t.loc\t" << FileNo << " " << Line << " " << Column;

  // Emit common suffix (flags, comment, EOL, parent call).
  emitDwarfLocDirectiveSuffix(FileNo, Line, Column, Flags, Isa, Discriminator,
                              FileName, Comment);
}

void NVPTXAsmStreamer::emitDwarfLocDirectiveWithInlinedAt(
    unsigned FileNo, unsigned Line, unsigned Column, unsigned FileIA,
    unsigned LineIA, unsigned ColIA, const MCSymbol *Sym, unsigned Flags,
    unsigned Isa, unsigned Discriminator, StringRef FileName,
    StringRef Comment) {
  // Emit the basic .loc directive with NVPTX-specific extensions.
  OS << "\t.loc\t" << FileNo << " " << Line << " " << Column;
  OS << ", function_name " << *Sym;
  OS << ", inlined_at " << FileIA << " " << LineIA << " " << ColIA;

  // Emit common suffix (flags, comment, EOL, parent call).
  emitDwarfLocDirectiveSuffix(FileNo, Line, Column, Flags, Isa, Discriminator,
                              FileName, Comment);
}

void NVPTXAsmStreamer::emitPTXDataType(NVPTXDataType Ty) {
  OS << ".";
  switch (Ty.Type) {
  case NVPTXDataType::BasicType::Unsigned: {
    OS << "u";
    break;
  }
  case NVPTXDataType::BasicType::Signed: {
    OS << "s";
    break;
  }
  case NVPTXDataType::BasicType::FloatingPoint: {
    OS << "f";
    break;
  }
  case NVPTXDataType::BasicType::Bits: {
    OS << "b";
    break;
  }
  case NVPTXDataType::BasicType::Predicate: {
    OS << "pred";
    break;
  }
  default:
    llvm_unreachable("Unexpected basic type in PTXDataType.");
  }

  if (!Ty.isPred())
    OS << Ty.BitSize;
}

void NVPTXAsmStreamer::emitLocalVariable(NVPTXDataType Ty, StringRef Name,
                                         unsigned Alignment,
                                         unsigned ArrElems) {
  OS << "\t.local ";
  emitAlignment(Alignment);
  OS << " ";
  emitPTXDataType(Ty);
  OS << " \t" << Name;
  if (ArrElems)
    OS << "[" << ArrElems << "]";
  OS << ";";

  EmitEOL();
}

void NVPTXAsmStreamer::emitRegisterVariable(NVPTXDataType Ty, StringRef Name,
                                            unsigned Parameter) {
  OS << "\t.reg ";
  emitPTXDataType(Ty);
  OS << " \t" << Name;
  if (Parameter)
    OS << "<" << Parameter << ">";
  OS << ";";

  EmitEOL();
}

static void emitPTXOpaqueType(PTXOpaqueType Ty, raw_ostream &OS) {
  switch (Ty) {
  case PTXOpaqueType::Sampler:
    OS << ".samplerref";
    break;
  case PTXOpaqueType::Texture:
    OS << ".texref";
    break;
  case PTXOpaqueType::Surface:
    OS << ".surfref";
    break;
  case PTXOpaqueType::None:
    llvm_unreachable("handled above");
  }
}

void NVPTXAsmStreamer::emitParameter(NVPTXFuncParam Param, bool PrefixTab) {
  if (PrefixTab)
    OS << "\t";
  OS << ".param ";

  // Speical handling for image/sampler parameters
  if (Param.OpType != PTXOpaqueType::None) {
    if (Param.EmitImgPtr) {
      emitPTXDataType(Param.Type);
      OS << " ";
      assert(Param.IsPtr && "Image/sampler parameter expected to be ptr");
      OS << ".ptr ";
    }

    emitPTXOpaqueType(Param.OpType, OS);
    OS << " ";

    OS << Param.Name;
    return;
  }

  if (Param.Alignment && !Param.IsPtr) {
    emitAlignment(Param.Alignment);
    OS << " ";
  }

  emitPTXDataType(Param.Type);
  OS << " ";

  if (Param.IsPtr) {
    OS << ".ptr ";
    emitPTXAddrSpace(Param.PtrAddrSpace);
    OS << " ";
    emitAlignment(Param.Alignment);
    OS << " ";
  }

  OS << Param.Name;
  if (Param.NumArrayElems)
    OS << "[" << Param.NumArrayElems << "]";
}

void NVPTXAsmStreamer::emitFunctionDecl(NVPTXFuncDecl PTXFunc) {
  emitLinkage(PTXFunc.Linkage);
  std::string FuncDir = PTXFunc.IsKernelFunc ? ".entry" : ".func";
  OS << " " << FuncDir << " ";

  if (PTXFunc.HasRetVal) {
    OS << "(";
    emitParameter(PTXFunc.RetVal);
    OS << ") ";
  }

  PTXFunc.Sym->print(OS, MAI);
  OS << "\n";

  if (PTXFunc.Params.empty()) {
    OS << "()";
  } else {
    OS << "(\n";
    for (auto [I, Param] : llvm::enumerate(PTXFunc.Params)) {
      if (I != 0)
        OS << ",\n";

      emitParameter(Param, true /*PrefixTab*/);
    }
    OS << "\n)";
  }
  OS << "\n";

  if (PTXFunc.EmitNoReturn)
    OS << ".noreturn";

  OS << ";\n";
}

void NVPTXAsmStreamer::emitOpaqueTyGlobal(NVPTXLinkage Linkage,
                                          PTXOpaqueType OpaqueType,
                                          StringRef Name,
                                          bool NeedInitForSampler,
                                          unsigned Sample) {
  emitLinkage(Linkage);
  OS << " ";
  // Opaque type GVs are always in global addrspace
  OS << ".global ";
  emitPTXOpaqueType(OpaqueType, OS);
  OS << " ";
  OS << Name;

  if (NeedInitForSampler) {
    OS << " = { ";

    for (int I = 0,
             Addr = ((Sample & __CLK_ADDRESS_MASK) >> __CLK_ADDRESS_BASE);
         I < 3; I++) {
      OS << "addr_mode_" << I << " = ";
      switch (Addr) {
      case 0:
        OS << "wrap";
        break;
      case 1:
        OS << "clamp_to_border";
        break;
      case 2:
        OS << "clamp_to_edge";
        break;
      case 3:
        OS << "wrap";
        break;
      case 4:
        OS << "mirror";
        break;
      }
      OS << ", ";
    }
    OS << "filter_mode = ";
    switch ((Sample & __CLK_FILTER_MASK) >> __CLK_FILTER_BASE) {
    case 0:
      OS << "nearest";
      break;
    case 1:
      OS << "linear";
      break;
    case 2:
      llvm_unreachable("Anisotropic filtering is not supported");
    default:
      OS << "nearest";
      break;
    }
    if (!((Sample & __CLK_NORMALIZED_MASK) >> __CLK_NORMALIZED_BASE)) {
      OS << ", force_unnormalized_coords = 1";
    }
    OS << " }";
  }

  OS << ";\n";
}

void NVPTXAsmStreamer::emitGlobalVariable(NVPTXLinkage Linkage,
                                          unsigned AddrSpace,
                                          bool HasAttrManaged,
                                          unsigned Alignment, NVPTXDataType Ty,
                                          MCSymbol *Sym, unsigned NumArrElems,
                                          bool HasInit) {
  emitLinkage(Linkage);
  OS << " ";
  emitPTXAddrSpace(AddrSpace);
  OS << " ";
  if (HasAttrManaged)
    OS << ".attribute(.managed) ";

  emitAlignment(Alignment);
  OS << " ";

  emitPTXDataType(Ty);
  OS << " ";

  Sym->print(OS, MAI);

  if (NumArrElems)
    OS << "[" << NumArrElems << "]";

  if (!HasInit) {
    OS << ";\n";
    return;
  }

  OS << " = ";
  if (NumArrElems)
    OS << "{";
}
