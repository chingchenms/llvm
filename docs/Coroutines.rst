=====================================
Coroutines in LLVM
=====================================

.. contents::
   :local:
   :depth: 3

Status
======

This document describes a set of experimental extensions to LLVM. Use
with caution.  Because the intrinsics have experimental status,
compatibility across LLVM releases is not guaranteed. These intrinsics
are added to support C++ Coroutine TS (P0057), though they are general enough 
to be used to implement coroutines in other languages as well.

Overview
========

LLVM coroutines are functions that have one or more `suspend points`_. 
When a suspend point is reached, execution of a coroutine is suspended. 
A suspended coroutine can be resumed to continue execution from the last 
suspend point or be destroyed. In the following example function `f` returns
a handle to a suspended coroutine (**coroutine handle**) that can be passed to 
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

.. _coroutine frame:

In addition to the stack frame which exists when a coroutine is executing, 
there is an additional region of storage that contains objects that keeps the 
coroutine state when a coroutine is suspended. This region of storage
is called **coroutine frame**. It is created when a coroutine is invoked.
It is destroyed when a coroutine runs to completion or destroyed. 

An LLVM coroutine is represented as an LLVM function that has calls to
`coroutine intrinsics`_ defining the structure of the coroutine.

.. marking up suspend points and coroutine frame 
   allocation and deallocation code. Marking up allocation and deallocation code 
   allows an optimization to remove allocation/deallocation when coroutine frame
   can be stored on a frame of the caller. 

After mandatory coroutine processing passes a coroutine is split into several
functions that represent three different ways of how control can enter the 
coroutine: an initial invocation that creates the coroutine frame and executes
the coroutine code until it encounters a suspend point or reaches the end
of the coroutine, coroutine resume function that contains the code to be 
executed once coroutine is resumed at a particular suspend point, and a 
coroutine destroy function that is invoked when the coroutine is destroyed.

Coroutines by Example
=====================

Coroutine Representation
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
    %frame = call i8* @llvm.experimental.coro.init(i8* %alloc, i32 0, i8* null, i8* null)
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
memory). We will examine the other parameters to `coro.init`_ later.

In the cleanup block `coro.delete` intrinsic, given the coroutine frame pointer,
returns a memory address to be freed.

Two other intrinsics seen in this fragment are used to mark up the control flow
during the initial and subsequent invocation of the coroutine. The true branch
of the conditional branch following the `coro.fork`_ intrinsic indicates the 
block where control flow should transfer on the first suspension of the
coroutine. The `coro.resume.end`_ intrinsic is a no-op during an 
initial invocation of the coroutine. When the coroutine resumes, the intrinsic
marks the point when coroutine need to return control back to the caller.

The `coro.return` block returns a pointer to a coroutine frame which happens to
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
    %suspend = call i1 @llvm.experimental.coro.suspend(token none, i1 false)
    br i1 %suspend, label %resume, label %cleanup

  resume:
    %inc = add i32 %n.val, 1
    br label %coro.start

When control reaches `coro.suspend`_ intrinsic, the coroutine is suspended.
The conditional branch following the `coro.suspend` intrinsic indicates two
alternative continuation for the coroutine, one for normal resume, another
for destroy. The boolean parameter to `coro.suspend` indicates whether a
suspend point represents a `final suspend`_ or not.

Coroutine Transformation
------------------------

In the coroutine shown in the previous section, use of virtual register `%n.val`
is separated from the definition by a suspend point, it cannot reside
on the stack frame of the coroutine since it will go away once coroutine is
suspended and therefore need to go into the coroutine frame.

Other members of the coroutine frame will be an address of a resume and destroy
functions representing the coroutine behavior that needs to happen when coroutine
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
    %0 = call nonnull i8* @llvm.experimental.coro.init(i8* %alloc, i32 0, i8* null, i8* null)
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
    %frame = call i8* @llvm.experimental.coro.init(i8* %phi, i32 0, i8* null, i8* null)

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

Let's consider the coroutine that has more than one suspend point:

.. code-block:: C++

  void *f(int n) {
     for(;;) {
       yield(n++);
       <suspend>
       yield(-n);
       <suspend>
     }
  }

Matching LLVM code would look like (with the rest of the code remaining the same
as the code in the previous section):

.. code-block:: llvm

  coro.start:
      %n.val = phi i32 [ %n, %coro.init ], [ %inc, %resume ]
      call void @yield(i32 %n.val)
      %suspend1 = call i1 @llvm.experimental.coro.suspend(token none, i1 false)
      br i1 %suspend1, label %resume, label %cleanup

    resume:
      %inc = add i32 %n.val, 1
      %sub = sub nsw i32 0, %inc
      call void @yield(i32 %sub)
      %suspend2 = call i1 @llvm.experimental.coro.suspend(token none, i1 false)
      br i1 %suspend2, label %coro.start, label %cleanup

In this case, coroutine frame would include a suspend index that will indicate
at which suspend point a coroutine needs to resume and `f.resume` function
will start with a switch as follows:

.. code-block:: llvm

  define internal fastcc void @f.resume(%f.frame* nocapture nonnull %frame.ptr.resume) {
  entry:
    %index.addr = getelementptr %f.frame, %f.frame* %frame.ptr.resume, i64 0, i32 2
    %index = load i32, i32* %0, align 4
    %switch = icmp eq i32 %index, 0
    br i1 %switch, label %resume, label %coro.start

  coro.start:
    ...
    br label %exit

  resume:
    ...
    br label %exit

  exit:
    %storemerge = phi i32 [ 1, %resume ], [ 0, %coro.start ]
    store i32 %storemerge, i32* %index.addr, align 4
    ret void
  }

If different cleanup code needs to get executed for different suspend points, 
a similar switch will be in the `f.destroy` function.

.. note ::

  Using suspend index in a coroutine state and having a switch in `f.resume` and
  `f.destroy` is one of the possible implementation strategies. We explored 
  another option where a distinct `f.resume1`, `f.resume2`, etc are created for
  every suspend point and instead of storing an index, the resume and destroy 
  function pointers are updated at every suspend. Early testing showed that the
  former is easier on the optimizer than the latter so it is a strategy 
  implemented at the moment.

Distinct Save and Suspend
-------------------------

In the previous example, setting a resume index (or some other state change that 
needs to happen to prepare coroutine for resumption) happens at the same time as
suspension of a coroutine. However, in certain cases it is necessary to control 
when coroutine is prepared for resumption and when it is suspended.

In the following example, coroutine represents some activity that is driven
by completions of asynchronous operations `async_op1` and `async_op2` which get
a coroutine handle as a parameter and will resume the coroutine once async
operation is finished.

.. code-block:: llvm

  void g() {
     for (;;)
       if (cond()) {
          async_op1(<coroutine-handle>); // will resume once async_op1 completes
          <suspend>
          do_one();
       }
       else {
          async_op2(<coroutine-handle>); // will resume once async_op2 completes
          <suspend>
          do_two();
       }
     }
  }

In this case, coroutine should be ready for resumption prior to a call to 
`async_op1` and `async_op2`. The `coro.save`_ intrinsic is used to indicate a
point when coroutine should be ready for resumption:

.. code-block:: llvm

  if.true:
    %save1 = call token @llvm.experimental.coro.save()
    call void async_op1(i8* %frame)
    %suspend1 = call i1 @llvm.experimental.coro.suspend(token %save1, i1 false)
    br i1 %suspend1, label %resume1, label %cleanup

  if.false:
    %save2 = call token @llvm.experimental.coro.save()
    call void async_op2(i8* %frame)
    %suspend2 = call i1 @llvm.experimental.coro.suspend(token %save2, i1 false)
    br i1 %suspend2, label %resume2, label %cleanup

.. _final:
.. _final suspend:

Final Suspend
-------------

.. Coroutines we considered so far do not complete on their own. They run
   until explicitly destroyed by the call to `coro.destroy`_. If we consider a case
   of a coroutine representing a generator that produces a finite sequence of

One of the common coroutine usage patterns is a generator, where a coroutine
produces a (sometime finite) sequence of values. To facilitate this pattern
frontend can designate a suspend point to be final. A coroutine suspended at
the final suspend point, can only be resumed with `coro.destroy`_ intrinsic.
Resuming such coroutine with `coro.resume`_ results in undefined behavior.
The `coro.done`_ intrinsic can be used to check whether a suspended coroutine
is at the final suspend point or not.

The following is an example of a function that keeps resuming the coroutine
until the final suspend point is reached after which point the coroutine is 
destroyed:

.. code-block:: llvm

  define i32 @main() {
  entry:
    %coro = call i8* @g()
    br %while.cond
  while.cond:
    %done = call i1 @llvm.experimental.coro.done(i8* %coro)
    br i1 %done, label %while.end, label %while.body
  while.body:
    call void @llvm.experimental.coro.resume(i8* %coro)
    br label %while.cond
  while.end:
    call void @llvm.experimental.coro.destroy(i8* %coro)
    ret i32 0
  }

.. _coroutine promise:

Coroutine Promise
-----------------

A coroutine author or a frontend may designate a distinguished `alloca` that can
be used to communicate with the coroutine. This distinguished alloca is called
**coroutine promise** and is provided as a third parameter to the `coro.init`_ 
intrinsic.

The following coroutine designates a 32 bit integer `promise` and uses it to
store the current value produces by a coroutine.

.. code-block:: llvm

  define i8* @f(i32 %n) {
  entry:
    %promise = alloca i32
    %pv = bitcast i32* %promise to i8*
    %frame.size = call i32 @llvm.experimental.coro.size()
    %alloc = call noalias i8* @malloc(i32 %frame.size)
    %frame = call i8* @llvm.experimental.coro.init(i8* %alloc, i32 0, i8* %pv, i8* null)
    %first.return = call i1 @llvm.experimental.coro.fork()
    br i1 %first.return, label %coro.return, label %coro.start

  coro.start:
    %n.val = phi i32 [ %n, %entry ], [ %inc, %resume ]
    store i32 %n.val, i32* %promise
    %suspend = call i1 @llvm.experimental.coro.suspend2(token none, i1 false)
    br i1 %suspend, label %resume, label %cleanup

  resume:
    %inc = add i32 %n.val, 1
    br label %coro.start

  cleanup:
    %mem = call i8* @llvm.experimental.coro.delete(i8* %frame)
    call void @free(i8* %mem)
    br label %coro.return

  coro.return:
    ret i8* %frame
  }

Coroutine consumer can rely on the `coro.promise`_ intrinsic to access the
coroutine promise.

.. code-block:: llvm

  define i32 @main() {
  entry:
    %hdl = call i8* @f(i32 4)
    %promise.addr = call i32* @llvm.experimental.coro.promise.p0i32(i8* %hdl)
    %val0 = load i32, i32* %promise.addr
    call void @yield(i32 %val0)
    call void @llvm.experimental.coro.resume(i8* %hdl)
    %val1 = load i32, i32* %promise.addr
    call void @yield(i32 %val1)
    call void @llvm.experimental.coro.resume(i8* %hdl)
    %val2 = load i32, i32* %promise.addr
    call void @yield(i32 %val2)
    call void @llvm.experimental.coro.destroy(i8* %hdl)
    ret i32 0
  }

There is also an intrinsic `coro.from.promise`_ that performs a reverse
operation. Given an address of a coroutine promise, it obtains a coroutine handle. 
This intrinsic is the only mechanism for a user code outside of the coroutine 
to get access to the coroutine handle.

Intrinsics
==========

Coroutine Manipulation Intrinsics
---------------------------------

Intrinsics described in this section are used to manipulate an existing
coroutine.

.. _coro.destroy:

'llvm.experimental.coro.destroy' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Syntax:
"""""""

::

      declare void @llvm.experimental.coro.destroy(i8* <handle>)

Overview:
"""""""""

The '``llvm.experimental.coro.destroy``' intrinsic destroys a suspended
coroutine.

Arguments:
""""""""""

The argument is a coroutine handle to a suspended coroutine.

Semantics:
""""""""""

If coroutine identity is known, the `coro.destroy` intrinsic is replaced with a
direct call to coroutine destroy function. Otherwise it is replaced with an
indirect call based on the function pointer for the destroy function stored 
in the coroutine frame. Destroying a coroutine that is not suspended results in
undefined behavior.

.. _coro.resume:

'llvm.experimental.coro.resume' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

      declare void @llvm.experimental.coro.resume(i8* <handle>)

Overview:
"""""""""

The '``llvm.experimental.coro.resume``' intrinsic resumes a suspended
coroutine.

Arguments:
""""""""""

The argument is a handle to a suspended coroutine.

Semantics:
""""""""""

If coroutine identity is known, the `coro.resume` intrinsic is replaced with a
direct call to coroutine resume function. Otherwise it is replaced with an
indirect call based on the function pointer for the resume function stored 
in the coroutine frame. Resuming a coroutine that is not suspended results in
undefined behavior.

.. _coro.done:

'llvm.experimental.coro.done' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

      declare i1 @llvm.experimental.coro.done(i8* <handle>)

Overview:
"""""""""

The '``llvm.experimental.coro.done``' intrinsic checks whether a suspended
coroutine is at the final suspend point or not.

Arguments:
""""""""""

The argument is a handle to a suspended coroutine.

Semantics:
""""""""""

Using this intrinsic on a coroutine that does not have a `final suspend`_ point 
or on a coroutine that is not suspended results in an undefined behavior.

.. _coro.promise:

'llvm.experimental.coro.promise' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

      declare <type>* @llvm.experimental.coro.promise.p0<type>(i8* <handle>)

Overview:
"""""""""

The '``llvm.experimental.coro.promise``' intrinsic returns an address of
a `coroutine promise`_.

Arguments:
""""""""""

The argument is a handle to a coroutine.

Semantics:
""""""""""

Using this intrinsic on a coroutine that does not have a coroutine promise
results in undefined behavior. It is possible to read and modify coroutine
promise of the coroutine which is currently executing. The coroutine author and
a coroutine user are responsible to makes sure there is no data races.

.. _coro.from.promise:

'llvm.experimental.coro.from.promise' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

::

    declare i8* @llvm.experimental.coro.from.promise.p0<type>(<type>* <handle>)

Overview:
"""""""""

The '``llvm.experimental.coro.from.promise``' intrinsic returns a coroutine
handle given the coroutine promise.

Arguments:
""""""""""

An address of a coroutine promise.

Semantics:
""""""""""

Using this intrinsic on a coroutine that does not have a coroutine promise
results in undefined behavior.

.. _coroutine intrinsics:

Coroutine Structure Intrinsics
------------------------------
Intrinsics described in this section are used within a coroutine to describe
the coroutine structure. They should not be used outside of a coroutine.

.. _coro.size:

'llvm.experimental.coro.size' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

    declare i32 @llvm.experimental.coro.size()
    declare i64 @llvm.experimental.coro.size()

Overview:
"""""""""

The '``llvm.experimental.coro.size``' intrinsic returns the number of bytes
required to store a `coroutine frame`_.

Arguments:
""""""""""

None.

Semantics:
""""""""""

The `coro.size` intrinsic is lowered to a constant representing the size of
the coroutine frame.

.. _coro.init:

'llvm.experimental.coro.init' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i8* @llvm.experimental.coro.init(i8* %mem, i32 %align, i8* %promise, i8* %fnaddr)

Overview:
"""""""""

The '``llvm.experimental.coro.init``' intrinsic returns an address of the 
coroutine frame.

Arguments:
""""""""""

The first argument is a pointer to a block of memory in which coroutine frame
will reside. This could be the result of an allocation function or the result of
a call to a `coro.elide`_ intrinsics representing a storage that can be used on a
frame of the calling function.

The second argument provides information on alignment of the memory returned by
the allocation function and given to `coro.init` by the first parameter. If this
argument is 0, the memory is assumed to be aligned to 2 * sizeof(i8*).
This argument only accepts constants.

The third argument, if not `null`, designates a particular alloca instruction to
be a `coroutine promise`_.

The fourth argument is a function pointer to a coroutine itself.
If this argument is `null`, CoroEarly pass will replace it
with an address of the enclosing function. 

.. note::
  Since `coro.init` intrinsic is not lowered until late optimizer passes, 
  `fnaddr` argument can be used to distinguish between `coro.init` that 
  describes a structure of a pre-split coroutine or a `coro.init` belonging to 
  a post-split coroutine that was inlined into a different function.

Semantics:
""""""""""

Depending on the alignment requirements of the objects in the coroutine frame
and/or on the codegen compactness reasons the pointer returned from `coro.init` 
may be at offset to the %mem% argument. (This could be beneficial if instructions
that express relative access to data can be more compactly encoded with small
positive and negative offsets).

Front-end should emit exactly one `coro.init` intrinsic per coroutine.
It should appear prior to `coro.fork`_ intrinsic.

.. _coro.delete:

'llvm.experimental.coro.delete' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i8* @llvm.experimental.coro.delete(i8* %frame)

Overview:
"""""""""

The '``llvm.experimental.coro.delete``' intrinsic returns a pointer to a block
of memory where coroutine frame is stored or `null` if the allocation
of the coroutine frame was elided.

Arguments:
""""""""""

A pointer to the coroutine frame. This should be the same pointer that was 
returned by prior `coro.init` call.

Example (allow heap allocation elision):
""""""""""""""""""""""""""""""""""""""""

.. code-block:: llvm

  cleanup:
    %mem = call i8* @llvm.experimental.coro.delete(i8* %frame)
    %tobool = icmp ne i8* %mem, null
    br i1 %tobool, label %if.then, label %if.end

  if.then:
    call void @free(i8* %mem)
    br label %if.end

  if.end:
    ret void

Example (no heap allocation elision):
""""""""""""""""""""""""""""""""""""""""

.. code-block:: llvm

  cleanup:
    %mem = call i8* @llvm.experimental.coro.delete(i8* %frame)
    call void @free(i8* %mem)
    ret void


.. _coro.elide:

'llvm.experimental.coro.elide' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i8* @llvm.experimental.coro.elide()

Overview:
"""""""""

The '``llvm.experimental.coro.frame``' intrinsic returns an address of the 
memory on the callers frame where coroutine frame of this coroutine can be 
placed and `null` otherwise.

Arguments:
""""""""""

None

Semantics:
""""""""""

If the coroutine is eligible for heap elision and the ramp function is inlined
in its caller, this intrinsic is lowered to an alloca storing the coroutine frame.
Otherwise, it is lowered to constant `null`.

Example:
""""""""""

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
    %frame = call i8* @llvm.experimental.coro.init(i8* %phi, i32 0, i8* null, i8* null)

.. _coro.frame:

'llvm.experimental.coro.frame' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i8* @llvm.experimental.coro.frame()

Overview:
"""""""""

The '``llvm.experimental.coro.init``' intrinsic returns an address of the 
coroutine frame.

Arguments:
""""""""""

None

Semantics:
""""""""""

This intrinsic is lowered to refer to the `coro.init`_ instruction. This is
a frontend convenience intrinsic that makes it easier to refer to the
coroutine frame during semantic analysis of the coroutine. This intrinsic maybe
removed in the future. 

.. _coro.fork:

'llvm.experimental.coro.fork' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i1 @llvm.experimental.coro.fork()

Overview:
"""""""""

The '``llvm.experimental.coro.fork``' intrinsic together with the conditional 
branch consuming the boolean value returned from this intrinsic is used to 
indicates where the control flows should transfer on the first suspension of the
coroutine. 

Arguments:
""""""""""

None

Semantics:
""""""""""
The true branch of the the conditional branch consuming the boolean value 
returned from this intrinsic indicate where the control flows should transfer on
the first suspension of the coroutine. 

This intrinsic is removed by the CoroSplit pass when all suspend points are
lowered.

.. _coro.resume.end:

'llvm.experimental.coro.resume.end' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare void @llvm.experimental.coro.resume.end()

Overview:
"""""""""

The '``llvm.experimental.coro.resume.end``' marks the point where execution
of the resume part of the coroutine should end and control returns back to 
the caller.


Arguments:
""""""""""

None

Semantics:
""""""""""
The `coro.resume.end`_ intrinsic is a no-op during an initial invocation of the 
coroutine. When the coroutine resumes, the intrinsic marks the point when 
coroutine need to return control back to the caller.

This intrinsic is removed by the CoroSplit pass when coroutine is split into
the start, resume and destroy parts. In start part, the intrinsic is removed,
in resume and destroy parts, it is replaced with `ret void` instructions and
the rest of the block containing `coro.resume.end` instruction is discarded.

.. _coro.suspend:
.. _suspend points:

'llvm.experimental.coro.suspend' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare i1 @llvm.experimental.coro.suspend(token %save, i1 %final)

Overview:
"""""""""

The '``llvm.experimental.coro.suspend``' marks the point where execution
of the coroutine need to get suspended and control returned back to the caller.
Conditional branch consuming the result of this intrinsic marks normal
and cleanup basic blocks that correspond to this suspend point.

Arguments:
""""""""""

The first argument refers to a token of `coro.save` intrinsic that marks the 
point when coroutine state is prepared for suspension. If `none` token is passed,
the intrinsic behaves as if there were a `coro.save` immediately preceding
the `coro.suspend` intrinsic.

The second argument indicates whether this suspension point is `final`_.
The second argument only accepts constants.

Semantics:
""""""""""

If a coroutine that was suspended at the suspend point marked by this intrinsic
is resumed via `coro.resume`_ the control will transfer to the basic block
marked by the true branch of the conditional branch consuming the result of the
`coro.suspend`. If it is resumed via `coro.destroy`_, it will proceed to the
false branch.

If suspend intrinsic is marked as final, it can consider the `true` branch
unreachable and can perform optimizations that can take advantage of that fact.

.. _coro.save:

'llvm.experimental.coro.save' Intrinsic
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
::

  declare token @llvm.experimental.coro.save()

Overview:
"""""""""

The '``llvm.experimental.coro.save``' marks the point where a coroutine 
is considered suspened (and thus eligible for resumption) but control
is not yet transferred back to the caller. Its return value should be consumed
by exactly one `coro.suspend` intrinsic that marks the point when control need
to be transferred to the coroutine's caller.

Arguments:
""""""""""

None

Semantics:
""""""""""

Whatever coroutine state changes are required to  enable resumption of
the coroutine from the corresponding suspend point should be done at the point of
`coro.save` intrinsic.

Example:
========
Separate save and suspend points are a necessity when coroutine is used to 
represent an asynchronous control flow driven by callbacks representing
completions of asynchronous operations.

In these cases, a coroutine should be ready for resumption prior to a call to 
`async_op` function that may trigger resumption of a coroutine from the same or
a different thread:

.. code-block:: llvm

    %save = call token @llvm.experimental.coro.save()
    call void async_op(i8* %frame)
    %suspend = call i1 @llvm.experimental.coro.suspend(token %save, i1 false)
    br i1 %suspend, label %resume, label %cleanup

Coroutine Transformation Passes
===============================
CoroEarly
---------
The pass CoroEarly lowers coroutine intrinsics that hide the details of the
structure of the coroutine frame, but, otherwise not needed to be preserved to
help later coroutine passes. This pass lowers `coro.frame`_, `coro.done`_, 
`coro.promise`_ and `coro.from.promise`_ intrinsics.

CoroInline
----------
Since coroutine transformation need to be done in the IPO order and inlining
pre-split coroutine is undesirable, the CoroInline pass wraps the inliner pass
to execute coroutine and inliner passes in the following order.

#. Call sites in the function `F` are inlined as appropriate
#. CoroElide pass is run on the function `F` to see if any coroutines were 
   inlined and are eligible for coroutine frame elision optimization.
#. If function `F` is a coroutine, resume and destroy parts are extracted into
   `F.resume` and `F.destroy` functions by the CoroSplit pass. 

CoroSplit
---------
The pass CoroSplit extracts resume and destroy parts into separate functions.

CoroElide
---------
The pass CoroElide examines if the inlined coroutine is eligible for heap 
allocation elision optimization. If so, it replaces `coro.elide` intrinsic with
an address of a coroutine frame placed on its caller and replaces
`coro.delete` intrinsics with null to remove the deallocation code. This pass
also replaces `coro.resume` and `coro.destroy` intrinsics with direct calls to
resume and destroy functions for a particular coroutine where possible.

CoroCleanup
-----------
This pass runs late to lower all coroutine related intrinsics not replaced by
earlier passes.

Areas Requiring Attention
=========================
#. Debug information is not supported at the moment.

#. A coroutine frame is bigger than it could be. Adding stack packing and stack 
   coloring like optimization on the coroutine frame will result in tighter
   coroutine frames.

#. The CoroElide optimization pass relies on coroutine ramp function to be
   inlined. It is possible to split the ramp function further to increase the
   likelihood that it will get inlined into its caller.

#. Design a convention that would make it possible to apply coroutine heap
   elision optimization across ABI boundaries.

#. Cannot handle coroutines with inalloca parameters (used in x86 on Windows)

#. Alignment is ignored by coro.init and coro.delete intrinsics.
