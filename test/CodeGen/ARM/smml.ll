; RUN: llc -mtriple=arm-eabi %s -o - | FileCheck %s -check-prefix=CHECK
; RUN: llc -mtriple=armv6-eabi %s -o - | FileCheck %s -check-prefix=CHECK-V6
; RUN: llc -mtriple=armv7-eabi %s -o - | FileCheck %s -check-prefix=CHECK-V7
; RUN: llc -mtriple=thumb-eabi %s -o - | FileCheck %s -check-prefix=CHECK-THUMB
; RUN: llc -mtriple=thumbv6-eabi %s -o - | FileCheck %s -check-prefix=CHECK-THUMB
; RUN: llc -mtriple=thumbv6t2-eabi %s -o - | FileCheck %s -check-prefix=CHECK-THUMBV6T2
; RUN: llc -mtriple=thumbv7-eabi %s -o - | FileCheck %s -check-prefix=CHECK-THUMBV7

define i32 @Test0(i32 %a, i32 %b, i32 %c) nounwind readnone ssp {
entry:
; CHECK-LABEL: Test0
; CHECK-NOT: smmls
; CHECK-V6-NOT: smmls
; CHECK-V7-NOT: smmls
; CHECK_THUMB-NOT: smmls
; CHECK-THUMBV6T2-NOT: smmls
; CHECK-THUMBV7-NOT: smmls
  %conv4 = zext i32 %a to i64
  %conv1 = sext i32 %b to i64
  %conv2 = sext i32 %c to i64
  %mul = mul nsw i64 %conv2, %conv1
  %shr5 = lshr i64 %mul, 32
  %sub = sub nsw i64 %conv4, %shr5
  %conv3 = trunc i64 %sub to i32
  ret i32 %conv3
}

define i32 @Test1(i32 %a, i32 %b, i32 %c) {
;CHECK-LABEL: Test1
;CHECK-NOT: smmls
;CHECK-THUMB-NOT: smmls
;CHECK-V6: smmls r0, [[Rn:r[1-2]]], [[Rm:r[1-2]]], r0
;CHECK-V7: smmls r0, [[Rn:r[1-2]]], [[Rm:r[1-2]]], r0
;CHECK-THUMBV6T2: smmls r0, [[Rn:r[1-2]]], [[Rm:r[1-2]]], r0
;CHECK-THUMBV7: smmls r0, [[Rn:r[1-2]]], [[Rm:r[1-2]]], r0
entry:
  %conv = sext i32 %b to i64
  %conv1 = sext i32 %c to i64
  %mul = mul nsw i64 %conv1, %conv
  %conv26 = zext i32 %a to i64
  %shl = shl nuw i64 %conv26, 32
  %sub = sub nsw i64 %shl, %mul
  %shr7 = lshr i64 %sub, 32
  %conv3 = trunc i64 %shr7 to i32
  ret i32 %conv3
}
