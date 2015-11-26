define i32 @main() #7 {
entry:
  %elided.frame = alloca %fib.frame
  %elided.vFrame = bitcast %fib.frame* %elided.frame to i8*
  %0 = bitcast i8* %elided.vFrame to void (%fib.frame*)**
  store void (%fib.frame*)* @fib.resume, void (%fib.frame*)** %0, align 8, !noalias !1
  %1 = getelementptr i8, i8* %elided.vFrame, i64 8
  %2 = bitcast i8* %1 to void (%fib.frame*)**
  store void (%fib.frame*)* @fib.cleanup, void (%fib.frame*)** %2, align 8, !noalias !1
  %3 = getelementptr i8, i8* %elided.vFrame, i64 16
  %4 = bitcast i8* %3 to i32*
  store i32 0, i32* %4, align 4, !noalias !1
  %5 = bitcast i8* %elided.vFrame to %fib.frame*
  call fastcc void @fib.resume(%fib.frame* %5)
  %6 = bitcast i8* %elided.vFrame to void (i8*)**
  %7 = load void (i8*)*, void (i8*)** %6, align 8
  %lnot.i7 = icmp eq void (i8*)* %7, null
  br i1 %lnot.i7, label %while.end, label %while.body.lr.ph

while.body.lr.ph:                                 ; preds = %entry
  %add.ptr.i.i = getelementptr inbounds i8, i8* %elided.vFrame, i64 20
  %current_value.i = bitcast i8* %add.ptr.i.i to i32*
  br label %while.body

while.body:                                       ; preds = %while.body.lr.ph, %while.body
  %sum.08 = phi i32 [ 0, %while.body.lr.ph ], [ %add, %while.body ]
  %8 = load i32, i32* %current_value.i, align 4, !tbaa !4
  %add = add nsw i32 %8, %sum.08
  %9 = bitcast i8* %elided.vFrame to %fib.frame*
  call fastcc void @fib.resume(%fib.frame* %9)
  %10 = load void (i8*)*, void (i8*)** %6, align 8
  %lnot.i = icmp eq void (i8*)* %10, null
  br i1 %lnot.i, label %while.end.thread, label %while.body

while.end.thread:                                 ; preds = %while.body
  %add.lcssa = phi i32 [ %add, %while.body ]
  %phitmp = add i32 %add.lcssa, -10
  br label %if.then.i.i

while.end:                                        ; preds = %entry
  %tobool.i.i.i = icmp eq i8* %elided.vFrame, null
  br i1 %tobool.i.i.i, label %_ZN5minigD1Ev.exit, label %if.then.i.i

if.then.i.i:                                      ; preds = %while.end.thread, %while.end
  %sum.0.lcssa10 = phi i32 [ %phitmp, %while.end.thread ], [ -10, %while.end ]
  %11 = bitcast i8* %elided.vFrame to %fib.frame*
  call fastcc void @fib.cleanup(%fib.frame* %11)
  br label %_ZN5minigD1Ev.exit

_ZN5minigD1Ev.exit:                               ; preds = %while.end, %if.then.i.i
  %sum.0.lcssa11 = phi i32 [ -10, %while.end ], [ %sum.0.lcssa10, %if.then.i.i ]
  ret i32 %sum.0.lcssa11
}
