# shared_memory
This header library aims at providing message passing / IPC via shared memory for all UNIX operating systems
supporting POSIX standard. It uses POSIX shmem, along with **named semaphores**; it's very portable, and the
use of named semaphores allows it to be used on Apple macOS (Darwin doesn't implement unnamed semaphores).
It starts by allocating a 4MB chunk via a single `ftruncate()` call and it's only allocated once throughout
the entire lifespan of the application. The single-call allocation ensures that it's usable on systems like
Darwin (on which ftruncate can only be called once) or systems that limit shared memory size to less than
4MB.

The message-passing semantics is implemented as a standard request-reply model. We require that once a publisher
publishes, the consumer **must** consume the published data before the publisher publishes more. The scheme is
guaranteed by two binary semaphores (a proof sketch is contained in the source file) that switch states.

For <=4MB transfers we use two memcpy calls to transfer the data in a single contiguous chunk along with size
information (so calls to our API is size-preserving; as long as they do not contain noncontiguous regions)

For >4MB transfers we manually page and transfer via multiple memcpys in a for loop in chunks of 4MBs. 

The 4MB limit is tunable via a macro contained in the source file, and it also controls the chunking size
of data transfers.

The application has been tested to perform well (and on-par with OpenMPI's shared memory implementation) in another
project aimed at building a high-performance fault-tolerant parallel processing framework.

However, it might still contain unknown bugs as it is only rudimantally tested. We recommend its use as a general
guide towards programming IPC/shared memory applications instead of an off-the-shelf API.

On Apple systems, repeated creation of shared memory handles containing the same name will result in a failure.
Please deallocate the shared memory region after use (or manually `rm -rf /dev/shm/<my_shared_memory>`).
On other systems, POSIX shared memory might require a special name format.
