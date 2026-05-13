; RUN: llc < %s -mtriple=nvptx64 -mcpu=sm_20 -emit-globals-ptx-streamer | FileCheck %s
; RUN: %if ptxas %{ llc < %s -mtriple=nvptx64 -mcpu=sm_20 -emit-globals-ptx-streamer | %ptxas-verify %}

; CHECK: .extern .func (.param .b32 func_retval0) bar
; CHECK: (
; CHECK:         .param .b32 bar_param_0,
; CHECK:         .param .b8 bar_param_1
; CHECK: )
; CHECK: ;
; CHECK: .extern .global .align 4 .u32 GblDecl;
; CHECK: .visible .global .align 1 .u8 Gbli08 = 17; // @kernel
; CHECK:                                         // 0x11
; CHECK: .visible .global .align 4 .u32 Gbli32 = 28824; // 0x7098
; CHECK: .visible .global .align 4 .u32 GblAi32[2] = {1, 2}; // 0x1
; CHECK:                                         // 0x2
; CHECK: .visible .global .align 2 .u8 GblVi08[2] = {1, 2}; // 0x1
; CHECK:                                         // 0x2

@GblDecl = external global i32
@Gbli08 = global i8 17
@Gbli32 = global i32 28824
@GblAi32 = global [2 x i32] [i32 1, i32 2]
@GblVi08 = global <2 x i8> <i8 1, i8 2>

declare float @bar(i32, i8) local_unnamed_addr

define ptx_kernel float @kernel() {
entry:
  %gep1 = getelementptr [2 x i32], ptr @GblAi32, i64 0, i64 0
  %e1 = load i32, ptr %gep1
  %ld1 = load <2 x i8>, ptr @GblVi08
  %e2 = extractelement <2 x i8> %ld1, i64 1
  %val = tail call float @bar(i32 %e1, i8 %e2)
  ret float %val
}
