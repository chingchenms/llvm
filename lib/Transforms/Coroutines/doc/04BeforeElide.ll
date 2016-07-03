%fib.frame = type { void (%fib.frame*)*, void (%fib.frame*)*, i32, %"struct.minig::promise_type", i32, %"struct.std::suspend_always", %"struct.std::suspend_always" }

; Function Attrs: alwaysinline nounwind
define void @fib(%struct.minig* noalias nocapture sret %agg.result) #0 {
_ZN5minigD1Ev.exit:
  %0 = tail call i1 @llvm.coro.suspend(i8* bitcast (void (%fib.frame*)* @fib.cleanup to i8*), i8* null, i1 true)
  %call = tail call noalias i8* @_Znwm(i64 32) #3
  %1 = tail call i8* @llvm.coro.init(i8* %call)
  %2 = ptrtoint i8* %1 to i64
  %3 = bitcast i8* %1 to void (%fib.frame*)**
  store void (%fib.frame*)* @fib.resume, void (%fib.frame*)** %3, align 8
  %4 = getelementptr i8, i8* %1, i64 8
  %5 = bitcast i8* %4 to void (%fib.frame*)**
  store void (%fib.frame*)* @fib.destroy, void (%fib.frame*)** %5, align 8
  %6 = getelementptr i8, i8* %1, i64 16
  %7 = bitcast i8* %6 to i32*
  store i32 0, i32* %7, align 4
  %8 = bitcast %struct.minig* %agg.result to i64*
  store i64 %2, i64* %8, align 8
  ret void
}

; Function Attrs: norecurse nounwind
define i32 @main() #7 {
entry:
  %0 = tail call i1 @llvm.coro.suspend(i8* bitcast (void (%fib.frame*)* @fib.cleanup to i8*), i8* null, i1 true) #3, !noalias !1
  %call.i = tail call noalias i8* @_Znwm(i64 32) #3, !noalias !1
  %1 = tail call i8* @llvm.coro.init(i8* %call.i) #3, !noalias !1
  %2 = bitcast i8* %1 to void (%fib.frame*)**
  store void (%fib.frame*)* @fib.resume, void (%fib.frame*)** %2, align 8, !noalias !1
  %3 = getelementptr i8, i8* %1, i64 8
  %4 = bitcast i8* %3 to void (%fib.frame*)**
  store void (%fib.frame*)* @fib.destroy, void (%fib.frame*)** %4, align 8, !noalias !1
  %5 = getelementptr i8, i8* %1, i64 16
  %6 = bitcast i8* %5 to i32*
  store i32 0, i32* %6, align 4, !noalias !1
  tail call void @llvm.coro.resume(i8* %1) #3
  %7 = bitcast i8* %1 to void (i8*)**
  %8 = load void (i8*)*, void (i8*)** %7, align 8
  %lnot.i7 = icmp eq void (i8*)* %8, null
  br i1 %lnot.i7, label %while.end, label %while.body.lr.ph

while.body.lr.ph:                                 ; preds = %entry
  %add.ptr.i.i = getelementptr inbounds i8, i8* %1, i64 20
  %current_value.i = bitcast i8* %add.ptr.i.i to i32*
  br label %while.body

while.body:                                       ; preds = %while.body.lr.ph, %while.body
  %sum.08 = phi i32 [ 0, %while.body.lr.ph ], [ %add, %while.body ]
  %9 = load i32, i32* %current_value.i, align 4, !tbaa !4
  %add = add nsw i32 %9, %sum.08
  tail call void @llvm.coro.resume(i8* %1) #3
  %10 = load void (i8*)*, void (i8*)** %7, align 8
  %lnot.i = icmp eq void (i8*)* %10, null
  br i1 %lnot.i, label %while.end.thread, label %while.body

while.end.thread:                                 ; preds = %while.body
  %add.lcssa = phi i32 [ %add, %while.body ]
  %phitmp = add i32 %add.lcssa, -10
  br label %if.then.i.i

while.end:                                        ; preds = %entry
  %tobool.i.i.i = icmp eq i8* %1, null
  br i1 %tobool.i.i.i, label %_ZN5minigD1Ev.exit, label %if.then.i.i

if.then.i.i:                                      ; preds = %while.end.thread, %while.end
  %sum.0.lcssa10 = phi i32 [ %phitmp, %while.end.thread ], [ -10, %while.end ]
  tail call void @llvm.coro.destroy(i8* nonnull %1) #3
  br label %_ZN5minigD1Ev.exit

_ZN5minigD1Ev.exit:                               ; preds = %while.end, %if.then.i.i
  %sum.0.lcssa11 = phi i32 [ -10, %while.end ], [ %sum.0.lcssa10, %if.then.i.i ]
  ret i32 %sum.0.lcssa11
}

