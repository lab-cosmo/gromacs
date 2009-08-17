

include(CheckIncludeFiles)

#option(THREAD_PTHREADS "Use posix threads" ON)

include(FindThreads)
if (CMAKE_USE_PTHREADS_INIT)
    check_include_files(pthread.h    HAVE_PTHREAD_H)
    #set(THREAD_PTHREADS 1)
    add_definitions(-DTHREAD_PTHREADS)
    set(THREAD_SRC pthreads.c)
    set(THREAD_LIB ${CMAKE_THREAD_LIBS_INIT})
else (CMAKE_USE_PTHREADS_INIT)
    if (CMAKE_USE_WIN32_THREADS_INIT)
        set(THREAD_WIN32 1)
        add_definitions(-DTHREAD_WIN32)
        set(THREAD_SRC winthreads.c)
        set(THREAD_LIBRARY )
    endif (CMAKE_USE_WIN32_THREADS_INIT)
endif (CMAKE_USE_PTHREADS_INIT)

