; RUN: opt < %s -analyze -delinearize | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.6.0"
; void foo(long n, long m, long o, double A[n][m][o], long p, long q, long r) {
;
;   for (long i = 0; i < n; i++)
;     for (long j = 0; j < m; j++)
;       for (long k = 0; k < o; k++)
;         A[i+p][j+q][k+r] = 1.0;
; }

; AddRec: {{{((8 * ((((%m * %p) + %q) * %o) + %r)) + %A),+,(8 * %m * %o)}<%for.i>,+,(8 * %o)}<%for.j>,+,8}<%for.k>
; CHECK: Base offset: %A
; CHECK: ArrayDecl[UnknownSize][%m][%o] with elements of 8 bytes.
; CHECK: ArrayRef[{%p,+,1}<nw><%for.i>][{%q,+,1}<nw><%for.j>][{%r,+,1}<nw><%for.k>]

; AddRec: {{(-8 + (8 * ((((%m * %p) + %q) * %o) + %r)) + (8 * %o) + %A),+,(8 * %m * %o)}<%for.i>,+,(8 * %o)}<%for.j>
; CHECK: Base offset: %A
; CHECK: ArrayDecl[UnknownSize][%o] with elements of 8 bytes.
; CHECK: ArrayRef[{(1 + (%m * %p) + %q),+,%m}<%for.i>][{(-1 + %r),+,%o}<%for.j>]

; AddRec: {(-8 + (8 * ((((%m * %p) + %q) * %o) + %r)) + (8 * %m * %o) + %A),+,(8 * %m * %o)}<%for.i>
; CHECK: Base offset: %A
; CHECK: ArrayDecl[UnknownSize] with elements of 8 bytes.
; CHECK: ArrayRef[{(-1 + ((((1 + %p) * %m) + %q) * %o) + %r),+,(%m * %o)}<%for.i>]

define void @foo(i64 %n, i64 %m, i64 %o, double* %A, i64 %p, i64 %q, i64 %r) {
entry:
  br label %for.i

for.i:
  %i = phi i64 [ 0, %entry ], [ %i.inc, %for.i.inc ]
  br label %for.j

for.j:
  %j = phi i64 [ 0, %for.i ], [ %j.inc, %for.j.inc ]
  br label %for.k

for.k:
  %k = phi i64 [ 0, %for.j ], [ %k.inc, %for.k.inc ]
  %offset0 = add nsw i64 %i, %p
  %subscript0 = mul i64 %offset0, %m
  %offset1 = add nsw i64 %j, %q
  %subscript1 = add i64 %offset1, %subscript0
  %subscript2 = mul i64 %subscript1, %o
  %offset2 = add nsw i64 %k, %r
  %subscript = add i64 %subscript2, %offset2
  %idx = getelementptr inbounds double* %A, i64 %subscript
  store double 1.0, double* %idx
  br label %for.k.inc

for.k.inc:
  %k.inc = add nsw i64 %k, 1
  %k.exitcond = icmp eq i64 %k.inc, %o
  br i1 %k.exitcond, label %for.j.inc, label %for.k

for.j.inc:
  %j.inc = add nsw i64 %j, 1
  %j.exitcond = icmp eq i64 %j.inc, %m
  br i1 %j.exitcond, label %for.i.inc, label %for.j

for.i.inc:
  %i.inc = add nsw i64 %i, 1
  %i.exitcond = icmp eq i64 %i.inc, %n
  br i1 %i.exitcond, label %end, label %for.i

end:
  ret void
}
