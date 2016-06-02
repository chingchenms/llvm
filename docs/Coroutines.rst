=====================================
Coroutines in LLVM
=====================================

.. contents::
   :local:
   :depth: 2

Status
======

This document describes a set of experimental extensions to LLVM. Use
with caution.  Because the intrinsics have experimental status,
compatibility across LLVM releases is not guaranteed.

.. note::

  TODO: mention C++ Coroutines and P0057

Overview
========

LLVM coroutines are functions that have one or more `suspend points`_. 
When a suspend point is reached, execution of a coroutine is suspended. 
A suspended coroutine can be resumed to continue execution from the last 
suspend point or be destroyed. In the following example function `f` returns
a handle to a suspended coroutine (`coroutine handle`_) that can be passed to 
`coro.resume`_ and `coro.destroy`_ intrinsics to resume and destroy the 
coroutine respectively.

.. code-block:: llvm

  define i32 @main() {
  entry:
    %hdl = call i8* @f(i32 4)
    call void @llvm.experimental.coro.resume(i8* %hdl)
    call void @llvm.experimental.coro.resume(i8* %hdl)
    call void @llvm.experimental.coro.destroy(i8* %hdl)
    ret i32 0
  }

In addition to the stack frame which exists when a coroutine is executing, 
there is an additional region of storage that contains objects that keeps the 
coroutine state when a coroutine is suspended. This region of storage
is called `coroutine frame`_. It is created when a coroutine is invoked.
It is destroyed when a coroutine runs to completion or destroyed. 

An LLVM coroutine is represented as an LLVM function with calls to set of 
coroutine specific intrinsics marking up suspend points and coroutine frame 
allocation and deallocation code. Marking up allocation and deallocation code 
allows an optimization to remove allocation/deallocation when coroutine frame
can be stored on a frame of the caller. 

After mandatory coroutine processing passes a coroutine is split into several
functions that represent three different way of how control can enter the 
coroutine: initial invocation that creates the coroutine frame and executes
the coroutine code until it encounters first suspend point or reaches the end
of the coroutine, coroutine resume function that contains the code to be 
executed once coroutine is resumed at a particular suspend point, and a 
coroutine destroy function that is invoked when coroutine is destroyed.

Coroutines by Example
=====================

Coroutine representation
------------------------

Let's look at an example of an LLVM coroutine with the behavior sketched
by the following pseudo-code.

.. code-block:: C++

  void *f(int n) {
     for(;;) {
       yield(n++);
       <suspend> // magic: returns coroutine handle on first suspend
     }
  }

This coroutine calls some function `yield` with value `n` as an argument and
suspends execution. Every time it resumes it calls `yield` again with an 
argument one bigger than the last time. This coroutine never completes by 
itself and must be destroyed explicitly. If we use this coroutine with 
a `main` shown in the previous section. It will call `yield` with values 4, 5 
and 6 after which the coroutine will be destroyed.

We will look at individual parts of the LLVM coroutine matching the pseudo-code
above starting with coroutine frame creating and destruction:

.. code-block:: llvm

  define i8* @f(i32 %n) {
  entry:
    %frame.size = call i32 @llvm.experimental.coro.size()
    %alloc = call i8* @malloc(i32 %frame.size)
    %frame = call i8* @llvm.experimental.coro.init(i8* %alloc, i8* null, i8* null)
    %first.return = call i1 @llvm.experimental.coro.fork()
    br i1 %first.return, label %coro.return, label %coro.start
  
  coro.start:
    ; ...
  resume:
    ; ...

  cleanup:
    %mem = call i8* @llvm.experimental.coro.delete(i8* %frame)
    call void @free(i8* %mem)
    call void @llvm.experimental.coro.resume.end()  
    br label %coro.return

  coro.return:
    ret i8* %frame
  }

First three lines of `entry` block establish the coroutine frame. The
`coro.size`_ intrinsic expands to represent the size required for the coroutine
frame. The `coro.init`_ intrinsic returns the address to be used as a coroutine
frame pointer (which could be at offset relative to the allocated block of
memory). We will examine the other two parameters to `coro.init`_ later.

In the cleanup block `coro.delete` intrinsic, given the coroutine frame pointer,
returns a memory address to be freed.

Two other intrinsics seen in this fragment are used to mark up the control flow
during the initial and subsequent invocation of the coroutine. The true branch
of the conditional branch following the `coro.fork`_ intrinsic indicates the 
block where control flow should transfer on the first suspension of the
coroutine or if control reaches the end of the function without encountering 
any suspend points. The `coro.resume.end`_ intrinsic is a no-op during an 
initial invocation of the coroutine. When the coroutine resumes, the intrinsic
marks the point when coroutine need to return control back to the caller.

The `coro.return` block returns a pointer to coroutine frame which happens to
be the same as `coroutine frame`_ expected by `coro.resume`_ and `coro.destroy`_
intrinsics.

.. The `malloc` function is used to allocate memory dynamically for 
.. coroutine frame.   

The rest of the coroutine code in blocks `coro.start` and `resume` 
is straightforward:

.. code-block:: llvm

  coro.start:
    %n.val = phi i32 [ %n, %entry ], [ %inc, %resume ]
    call void @yield(i32 %n.val)
    %suspend = call i1 @llvm.experimental.coro.suspend(token none)
    br i1 %suspend, label %resume, label %cleanup

  resume:
    %inc = add i32 %n.val, 1
    br label %coro.start

When control reaches `coro.suspend`_ intrinsic, the coroutine is suspended.
The conditional branch following the `coro.suspend` intrinsic indicates two
alternative continuation for the coroutine, one for normal resume, another
for destroy.

Coroutine Transformation
------------------------

In the coroutine shown in the previous section, use of virtual register `%n.val`
is separated from the definition by a suspend point, thus, it cannot reside
on the stack frame of the coroutine and need to go into the coroutine frame.

Other members of the coroutine frame will be an address of a resume and destroy
functions representing the instructions that needs to be executed when coroutine
is resumed and destroyed respectively.

.. code-block:: llvm

  %f.frame = type { void (%f.frame*)*, void (%f.frame*)*, i32 }

After coroutine transformation function `f` is responsible for creation and
initialization of the coroutine frame and execution of the coroutine code until
any suspend point is reached or control reaches the end of the function. It will
look like:

.. code-block:: llvm

  define i8* @f(i32 %n) {
  entry:
    %alloc = call noalias i8* @malloc(i32 24)
    %0 = call nonnull i8* @llvm.experimental.coro.init(i8* %alloc, i8* null, i8* null)
    %frame = bitcast i8* %frame to %f.frame*
    %1 = getelementptr %f.frame, %f.frame* %frame, i32 0, i32 0
    store void (%f.frame*)* @f.resume, void (%f.frame*)** %1
    %2 = getelementptr %f.frame, %f.frame* %frame, i32 0, i32 1
    store void (%f.frame*)* @f.destroy, void (%f.frame*)** %2
   
    %n.val.addr = getelementptr %f.frame, %f.frame* %frame, i32 0, i32 2
    store i32 %n, i32* %n.val.addr
    call void @yield(i32 %n)
   
    ret i8* %frame
  }

Part of the orginal coroutine `f` that is responsible for executing code after 
resume will be extracted into `f.resume` function:

.. code-block:: llvm

  define internal fastcc void @f.resume(%f.frame* %frame.ptr.resume) {
  entry:
    %n.val.addr = getelementptr %f.frame, %f.frame* %frame.ptr.resume, i64 0, i32 2
    %n.val = load i32, i32* %n.val.addr, align 4
    %inc = add i32 %n.val, 1
    store i32 %inc, i32* %n.val.addr, align 4
    tail call void @yield(i32 %inc)
    ret void
  }

Whereas function `f.destroy` will end up simply calling `free` function:

.. code-block:: llvm

  define internal fastcc void @f.destroy(%f.frame* %frame.ptr.destroy) {
  entry:
    %0 = bitcast %f.frame* %frame.ptr.destroy to i8*
    tail call void @free(i8* %0)
    ret void
  }

This transformation is performed by `coro-split` LLVM pass.

Avoiding Heap Allocations
-------------------------
 
A particular coroutine usage pattern which is illustrated by the `main` function
in the overview section where a coroutine is created, manipulated and destroyed by
the same calling function is common for generator coroutines and is suitable for
allocation elision optimization which stores coroutine frame in the caller's 
frame.

To enable heap elision, we need to make frame allocation and deallocation 
as follows:

In the entry block, we will invoke `coro.elide`_ intrinsic that will return 
an address of a coroutine frame on the callers if possible and `null` otherwise:

.. code-block:: llvm

  entry:
    %elide = call i8* @llvm.experimental.coro.elide()
    %0 = icmp ne i8* %elide, null
    br i1 %0, label %coro.init, label %coro.alloc

  coro.alloc:
    %frame.size = call i32 @llvm.experimental.coro.size()
    %alloc = call i8* @malloc(i32 %frame.size)
    br label %coro.init

  coro.init:
    %phi = phi i8* [ %elide, %entry ], [ %alloc, %coro.alloc ]
    %frame = call i8* @llvm.experimental.coro.init(i8* %phi, i8* null, i8* null)

In the cleanup block, we will make freeing the coroutine frame conditional on
`coro.delete`_ intrinsic. If allocation is elided, `coro.delete`_ returns `null`
thus avoiding deallocation code:

.. code-block:: llvm

  cleanup:
    %mem = call i8* @llvm.experimental.coro.delete(i8* %frame)
    %tobool = icmp ne i8* %mem, null
    br i1 %tobool, label %if.then, label %if.end

  if.then:
    call void @free(i8* %mem)
    br label %if.end

  if.end:
    call void @llvm.experimental.coro.resume.end()
    br label %coro.return

With allocations and deallocations described as above after inlining and heap
allocation elision optimization the resulting main will end up looking as:

.. code-block:: llvm

  define i32 @main() {
  entry:
    call void @yield(i32 4)
    call void @yield(i32 5)
    call void @yield(i32 6)
    ret i32 0
  }

Multiple Suspend Points
-----------------------

The coroutine we looked at so far was rather simplistic it only had one suspend
point and therefore execution would always

Distinct Save and Suspend
-------------------------

Reaching Inside
---------------

Final Suspend
-------------




Terms
=====
**Coroutine Handle**
  a pointer that encodes information about an



High Level Structure
====================

.. _suspend point:
.. _suspend points:

Suspend point
-------------
bla bla

.. _coroutine frame:

Coroutine frame
---------------
bla bla

.. _coroutine handle:

Coroutine Handle
----------------
bla bla

Intrinsics
==========

.. _coro.destroy:

'llvm.experimental.coro.destroy' Intrinsic
------------------------------------------
bla bla

.. _coro.delete:

'llvm.experimental.coro.delete' Intrinsic
-----------------------------------------
bla bla

.. _coro.resume:

'llvm.experimental.coro.resume' Intrinsic
-----------------------------------------
bla bla

.. _coro.done:

'llvm.experimental.coro.done' Intrinsic
-----------------------------------------
bla bla

.. _coro.size:

'llvm.experimental.coro.size' Intrinsic
---------------------------------------
bla bla

.. _coro.init:

'llvm.experimental.coro.init' Intrinsic
---------------------------------------
bla bla

.. _coro.fork:

'llvm.experimental.coro.fork' Intrinsic
---------------------------------------
bla bla

.. _coro.resume.end:

'llvm.experimental.coro.resume.end' Intrinsic
---------------------------------------------
bla bla

.. _coro.suspend:

'llvm.experimental.coro.suspend' Intrinsic
------------------------------------------
bla bla

.. _coro.save:

'llvm.experimental.coro.save' Intrinsic
---------------------------------------
bla bla

.. _coro.elide:

'llvm.experimental.coro.elide' Intrinsic
----------------------------------------
bla bla

