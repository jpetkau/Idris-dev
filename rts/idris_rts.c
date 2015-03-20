#include <assert.h>

#include "idris_rts.h"
#include "idris_gc.h"
#include "idris_bitstring.h"

#ifdef HAS_PTHREAD
static pthread_key_t vm_key;
#else
static VM* global_vm;
#endif

void free_key(VM *vm) {
    // nothing to free, we just used the VM pointer which is freed elsewhere
}

VM* init_vm(int stack_size, size_t heap_size,
            int max_threads // not implemented yet
            ) {

    VM* vm = malloc(sizeof(VM));
    STATS_INIT_STATS(vm->stats)
    STATS_ENTER_INIT(vm->stats)

    VAL* valstack = malloc(stack_size * sizeof(VAL));

    vm->valstack = valstack;
    vm->valstack_top = valstack;
    vm->valstack_base = valstack;
    vm->stack_max = valstack + stack_size;

    alloc_heap(&(vm->heap), heap_size, heap_size, NULL);

    vm->ret = NULL;
    vm->reg1 = NULL;
#ifdef HAS_PTHREAD
    vm->inbox = malloc(1024*sizeof(VAL));
    memset(vm->inbox, 0, 1024*sizeof(VAL));
    vm->inbox_end = vm->inbox + 1024;
    vm->inbox_write = vm->inbox;

    // The allocation lock must be reentrant. The lock exists to ensure that
    // no memory is allocated during the message sending process, but we also
    // check the lock in calls to allocate.
    // The problem comes when we use requireAlloc to guarantee a chunk of memory
    // first: this sets the lock, and since it is not reentrant, we get a deadlock.
    pthread_mutexattr_t rec_attr;
    pthread_mutexattr_init(&rec_attr);
    pthread_mutexattr_settype(&rec_attr, PTHREAD_MUTEX_RECURSIVE);

    pthread_mutex_init(&(vm->inbox_lock), NULL);
    pthread_mutex_init(&(vm->inbox_block), NULL);
    pthread_mutex_init(&(vm->alloc_lock), &rec_attr);
    pthread_cond_init(&(vm->inbox_waiting), NULL);

    vm->max_threads = max_threads;
    vm->processes = 0;

#else
    global_vm = vm;
#endif
    STATS_LEAVE_INIT(vm->stats)
    return vm;
}

VM* idris_vm() {
    VM* vm = init_vm(4096000, 4096000, 1);
    init_threadkeys();
    init_threaddata(vm);
    init_gmpalloc();
    init_nullaries();

    return vm;
}

void close_vm(VM* vm) {
    terminate(vm);
}

void init_threadkeys() {
#ifdef HAS_PTHREAD
    pthread_key_create(&vm_key, (void*)free_key);
#endif
}

void init_threaddata(VM *vm) {
#ifdef HAS_PTHREAD
    pthread_setspecific(vm_key, vm);
#endif
}

Stats terminate(VM* vm) {
    Stats stats = vm->stats;
    STATS_ENTER_EXIT(stats)
#ifdef HAS_PTHREAD
    free(vm->inbox);
#endif
    free(vm->valstack);
    free_heap(&(vm->heap));
#ifdef HAS_PTHREAD
    pthread_mutex_destroy(&(vm -> inbox_lock));
    pthread_mutex_destroy(&(vm -> inbox_block));
    pthread_cond_destroy(&(vm -> inbox_waiting));
#endif
    free(vm);

    STATS_LEAVE_EXIT(stats)
    return stats;
}

void idris_requireAlloc(size_t size) {
#ifdef HAS_PTHREAD
    VM* vm = pthread_getspecific(vm_key);
#else
    VM* vm = global_vm;
#endif

    if (!(vm->heap.next + size < vm->heap.end)) {
        idris_gc(vm);
    }
#ifdef HAS_PTHREAD
    int lock = vm->processes > 0;
    if (lock) { // We only need to lock if we're in concurrent mode
       pthread_mutex_lock(&vm->alloc_lock);
    }
#endif
}

void idris_doneAlloc() {
#ifdef HAS_PTHREAD
    VM* vm = pthread_getspecific(vm_key);
    int lock = vm->processes > 0;
    if (lock) { // We only need to lock if we're in concurrent mode
       pthread_mutex_unlock(&vm->alloc_lock);
    }
#endif
}

int space(VM* vm, size_t size) {
    return (vm->heap.next + size + sizeof(size_t) < vm->heap.end);
}

void* idris_alloc(size_t size) {
    return allocate(size, 0);
}

void* idris_realloc(void* old, size_t old_size, size_t size) {
    void* ptr = idris_alloc(size);
    memcpy(ptr, old, old_size);
    return ptr;
}

void idris_free(void* ptr, size_t size) {
}

void* allocate(size_t size, int outerlock) {
//    return malloc(size);

#ifdef HAS_PTHREAD
    VM* vm = pthread_getspecific(vm_key);
    int lock = vm->processes > 0 && !outerlock;

    if (lock) { // not message passing
       pthread_mutex_lock(&vm->alloc_lock);
    }
#else
    VM* vm = global_vm;
#endif

    if ((size & 7)!=0) {
	size = 8 + ((size >> 3) << 3);
    }

    size_t chunk_size = size + sizeof(size_t);

    if (vm->heap.next + chunk_size < vm->heap.end) {
        STATS_ALLOC(vm->stats, chunk_size)
        void* ptr = (void*)(vm->heap.next + sizeof(size_t));
        *((size_t*)(vm->heap.next)) = chunk_size;
        vm->heap.next += chunk_size;

        assert(vm->heap.next <= vm->heap.end);

        memset(ptr, 0, size);
#ifdef HAS_PTHREAD
        if (lock) { // not message passing
           pthread_mutex_unlock(&vm->alloc_lock);
        }
#endif
        return ptr;
    } else {
        idris_gc(vm);
#ifdef HAS_PTHREAD
        if (lock) { // not message passing
           pthread_mutex_unlock(&vm->alloc_lock);
        }
#endif
        return allocate(size, 0);
    }

}

/* Now a macro
void* allocCon(VM* vm, int arity, int outer) {
    Closure* cl = allocate(vm, sizeof(Closure) + sizeof(VAL)*arity,
                               outer);
    SETTY(cl, CON);

    cl -> info.c.arity = arity;
//    cl -> info.c.tag = 42424242;
//    printf("%p\n", cl);
    return (void*)cl;
}
*/

VAL MKFLOAT(VM* vm, double val) {
    Closure* cl = allocate(sizeof(Closure), 0);
    SETTY(cl, FLOAT);
    cl -> info.f = val;
    return cl;
}

VAL MKSTR(VM* vm, const char* str) {
    int len;
    if (str == NULL) {
        len = 0;
    } else {
        len = strlen(str)+1;
    }
    Closure* cl = allocate(sizeof(Closure) + // Type) + sizeof(char*) +
                           sizeof(char)*len, 0);
    SETTY(cl, STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    if (str == NULL) {
        cl->info.str = NULL;
    } else {
        strcpy(cl -> info.str, str);
    }
    return cl;
}

char* GETSTROFF(VAL stroff) {
    // Assume STROFF
    StrOffset* root = stroff->info.str_offset;
    return (root->str->info.str + root->offset);
}

VAL MKPTR(VM* vm, void* ptr) {
    Closure* cl = allocate(sizeof(Closure), 0);
    SETTY(cl, PTR);
    cl -> info.ptr = ptr;
    return cl;
}

VAL MKMPTR(VM* vm, void* ptr, size_t size) {
    Closure* cl = allocate(sizeof(Closure) +
                           sizeof(ManagedPtr) + size, 0);
    SETTY(cl, MANAGEDPTR);
    cl->info.mptr = (ManagedPtr*)((char*)cl + sizeof(Closure));
    cl->info.mptr->data = (char*)cl + sizeof(Closure) + sizeof(ManagedPtr);
    memcpy(cl->info.mptr->data, ptr, size);
    cl->info.mptr->size = size;
    return cl;
}

VAL MKFLOATc(VM* vm, double val) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, FLOAT);
    cl -> info.f = val;
    return cl;
}

VAL MKSTRc(VM* vm, char* str) {
    Closure* cl = allocate(sizeof(Closure) + // Type) + sizeof(char*) +
                           sizeof(char)*strlen(str)+1, 1);
    SETTY(cl, STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);

    strcpy(cl -> info.str, str);
    return cl;
}

VAL MKPTRc(VM* vm, void* ptr) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, PTR);
    cl -> info.ptr = ptr;
    return cl;
}

VAL MKMPTRc(VM* vm, void* ptr, size_t size) {
    Closure* cl = allocate(sizeof(Closure) +
                           sizeof(ManagedPtr) + size, 1);
    SETTY(cl, MANAGEDPTR);
    cl->info.mptr = (ManagedPtr*)((char*)cl + sizeof(Closure));
    cl->info.mptr->data = (char*)cl + sizeof(Closure) + sizeof(ManagedPtr);
    memcpy(cl->info.mptr->data, ptr, size);
    cl->info.mptr->size = size;
    return cl;
}

VAL MKB8(VM* vm, uint8_t bits8) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, BITS8);
    cl -> info.bits8 = bits8;
    return cl;
}

VAL MKB16(VM* vm, uint16_t bits16) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, BITS16);
    cl -> info.bits16 = bits16;
    return cl;
}

VAL MKB32(VM* vm, uint32_t bits32) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, BITS32);
    cl -> info.bits32 = bits32;
    return cl;
}

VAL MKB64(VM* vm, uint64_t bits64) {
    Closure* cl = allocate(sizeof(Closure), 1);
    SETTY(cl, BITS64);
    cl -> info.bits64 = bits64;
    return cl;
}

VAL MKB8x16const(VM* vm,
                 uint8_t v0,  uint8_t v1,  uint8_t v2,  uint8_t v3,
                 uint8_t v4,  uint8_t v5,  uint8_t v6,  uint8_t v7,
                 uint8_t v8,  uint8_t v9,  uint8_t v10, uint8_t v11,
                 uint8_t v12, uint8_t v13, uint8_t v14, uint8_t v15) {
    Closure* cl = allocate(sizeof(Closure) + sizeof(__m128i), 1);
    SETTY(cl, BITS8X16);

    cl->info.bits128p = (__m128i*)ALIGN((uintptr_t)cl + sizeof(Closure), 16);
    assert ((uintptr_t)cl->info.bits128p % 16 == 0);

    uint8_t data[16];
    data[0]=v0;   data[1]=v1;   data[2]=v2;   data[3]=v3;
    data[4]=v4;   data[5]=v5;   data[6]=v6;   data[7]=v7;
    data[8]=v8;   data[9]=v9;   data[10]=v10; data[11]=v11;
    data[12]=v12; data[13]=v13; data[14]=v14; data[15]=v15;

    *cl->info.bits128p = _mm_loadu_si128((__m128i*)&data);

    return cl;
}

VAL MKB8x16(VM* vm,
            VAL v0,  VAL v1,  VAL v2,  VAL v3,
            VAL v4,  VAL v5,  VAL v6,  VAL v7,
            VAL v8,  VAL v9,  VAL v10, VAL v11,
            VAL v12, VAL v13, VAL v14, VAL v15) {
    return MKB8x16const(vm,
        v0->info.bits8,  v1->info.bits8,  v2->info.bits8,  v3->info.bits8,
        v4->info.bits8,  v5->info.bits8,  v6->info.bits8,  v7->info.bits8,
        v8->info.bits8,  v9->info.bits8,  v10->info.bits8, v11->info.bits8,
        v12->info.bits8, v13->info.bits8, v14->info.bits8, v15->info.bits8);
}

VAL MKB16x8const(VM* vm,
                 uint16_t v0, uint16_t v1, uint16_t v2, uint16_t v3,
                 uint16_t v4, uint16_t v5, uint16_t v6, uint16_t v7) {
    Closure* cl = allocate(sizeof(Closure) + sizeof(__m128i), 1);
    SETTY(cl, BITS16X8);

    cl->info.bits128p = (__m128i*)ALIGN((uintptr_t)cl + sizeof(Closure), 16);
    assert ((uintptr_t)cl->info.bits128p % 16 == 0);

    uint16_t data[8];
    data[0]=v0; data[1]=v1; data[2]=v2; data[3]=v3;
    data[4]=v4; data[5]=v5; data[6]=v6; data[7]=v7;

    *cl->info.bits128p = _mm_loadu_si128((__m128i*)&data);
    return cl;

}

VAL MKB16x8(VM* vm,
            VAL v0, VAL v1, VAL v2, VAL v3,
            VAL v4, VAL v5, VAL v6, VAL v7) {
    return MKB16x8const(vm,
        v0->info.bits16, v1->info.bits16, v2->info.bits16, v3->info.bits16,
        v4->info.bits16, v5->info.bits16, v6->info.bits16, v7->info.bits16);
}

VAL MKB32x4const(VM* vm,
                 uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) {
    Closure* cl = allocate(sizeof(Closure) + 16 + sizeof(__m128i), 1);
    SETTY(cl, BITS64X2);

    cl->info.bits128p = (__m128i*)ALIGN((uintptr_t)cl + sizeof(Closure), 16);
    assert ((uintptr_t)cl->info.bits128p % 16 == 0);

    uint32_t data[4];
    data[0]=v0; data[1]=v1; data[2]=v2; data[3]=v3;

    *cl->info.bits128p = _mm_loadu_si128((__m128i*)&data);
    return cl;
}

VAL MKB32x4(VM* vm,
            VAL v0, VAL v1, VAL v2, VAL v3) {
    return MKB32x4const(vm,
        v0->info.bits32, v1->info.bits32, v2->info.bits32, v3->info.bits32);
}

VAL MKB64x2const(VM* vm, uint64_t v0, uint64_t v1) {
    Closure* cl = allocate(sizeof(Closure) + 16 + sizeof(__m128i), 1);
    SETTY(cl, BITS64X2);

    cl->info.bits128p = (__m128i*)ALIGN((uintptr_t)cl + sizeof(Closure), 16);
    assert ((uintptr_t)cl->info.bits128p % 16 == 0);

    uint64_t data[2];
    data[0]=v0; data[1]=v1;

    *cl->info.bits128p = _mm_loadu_si128((__m128i*)&data);
    return cl;
}

VAL MKB64x2(VM* vm, VAL v0, VAL v1) {
    return MKB64x2const(vm,
        v0->info.bits64,
        v1->info.bits64);
}

void PROJECT(VM* vm, VAL r, int loc, int arity) {
    int i;
    for(i = 0; i < arity; ++i) {
        LOC(i+loc) = r->info.c.args[i];
    }
}

void SLIDE(VM* vm, int args) {
    int i;
    for(i = 0; i < args; ++i) {
        LOC(i) = TOP(i);
    }
}

void dumpStack(VM* vm) {
    int i = 0;
    VAL* root;

    for (root = vm->valstack; root < vm->valstack_top; ++root, ++i) {
        printf("%d: ", i);
        dumpVal(*root);
        if (*root >= (VAL)(vm->heap.heap) && *root < (VAL)(vm->heap.end)) { printf("OK"); }
        printf("\n");
    }
    printf("RET: ");
    dumpVal(vm->ret);
    printf("\n");
}

void dumpVal(VAL v) {
    if (v==NULL) return;
    int i;
    if (ISINT(v)) {
        printf("%d ", (int)(GETINT(v)));
        return;
    }
    switch(GETTY(v)) {
    case CON:
        printf("%d[", TAG(v));
        for(i = 0; i < ARITY(v); ++i) {
            dumpVal(v->info.c.args[i]);
        }
        printf("] ");
        break;
    case STRING:
        printf("STR[%s]", v->info.str);
        break;
    case FWD:
        printf("FWD ");
        dumpVal((VAL)(v->info.ptr));
        break;
    default:
        printf("val");
    }

}

void idris_memset(void* ptr, i_int offset, uint8_t c, i_int size) {
    memset(((uint8_t*)ptr) + offset, c, size);
}

uint8_t idris_peek(void* ptr, i_int offset) {
    return *(((uint8_t*)ptr) + offset);
}

void idris_poke(void* ptr, i_int offset, uint8_t data) {
    *(((uint8_t*)ptr) + offset) = data;
}

void idris_memmove(void* dest, void* src, i_int dest_offset, i_int src_offset, i_int size) {
    memmove(dest + dest_offset, src + src_offset, size);
}

VAL idris_castIntStr(VM* vm, VAL i) {
    int x = (int) GETINT(i);
    Closure* cl = allocate(sizeof(Closure) + sizeof(char)*16, 0);
    SETTY(cl, STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    sprintf(cl -> info.str, "%d", x);
    return cl;
}

VAL idris_castBitsStr(VM* vm, VAL i) {
    Closure* cl;
    ClosureType ty = i->ty;

    switch (ty) {
    case BITS8:
        // max length 8 bit unsigned int str 3 chars (256)
        cl = allocate(sizeof(Closure) + sizeof(char)*4, 0);
        cl->info.str = (char*)cl + sizeof(Closure);
        sprintf(cl->info.str, "%" PRIu8, (uint8_t)i->info.bits8);
        break;
    case BITS16:
        // max length 16 bit unsigned int str 5 chars (65,535)
        cl = allocate(sizeof(Closure) + sizeof(char)*6, 0);
        cl->info.str = (char*)cl + sizeof(Closure);
        sprintf(cl->info.str, "%" PRIu16, (uint16_t)i->info.bits16);
        break;
    case BITS32:
        // max length 32 bit unsigned int str 10 chars (4,294,967,295)
        cl = allocate(sizeof(Closure) + sizeof(char)*11, 0);
        cl->info.str = (char*)cl + sizeof(Closure);
        sprintf(cl->info.str, "%" PRIu32, (uint32_t)i->info.bits32);
        break;
    case BITS64:
        // max length 64 bit unsigned int str 20 chars (18,446,744,073,709,551,615)
        cl = allocate(sizeof(Closure) + sizeof(char)*21, 0);
        cl->info.str = (char*)cl + sizeof(Closure);
        sprintf(cl->info.str, "%" PRIu64, (uint64_t)i->info.bits64);
        break;
    default:
        fprintf(stderr, "Fatal Error: ClosureType %d, not an integer type", ty);
        exit(EXIT_FAILURE);
    }

    SETTY(cl, STRING);
    return cl;
}

VAL idris_castStrInt(VM* vm, VAL i) {
    char *end;
    i_int v = strtol(GETSTR(i), &end, 10);
    if (*end == '\0' || *end == '\n' || *end == '\r')
        return MKINT(v);
    else
        return MKINT(0);
}

VAL idris_castFloatStr(VM* vm, VAL i) {
    Closure* cl = allocate(sizeof(Closure) + sizeof(char)*32, 0);
    SETTY(cl, STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    sprintf(cl -> info.str, "%g", GETFLOAT(i));
    return cl;
}

VAL idris_castStrFloat(VM* vm, VAL i) {
    return MKFLOAT(vm, strtod(GETSTR(i), NULL));
}

VAL idris_concat(VM* vm, VAL l, VAL r) {
    char *rs = GETSTR(r);
    char *ls = GETSTR(l);
    // dumpVal(l);
    // printf("\n");
    Closure* cl = allocate(sizeof(Closure) + strlen(ls) + strlen(rs) + 1, 0);
    SETTY(cl, STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    strcpy(cl -> info.str, ls);
    strcat(cl -> info.str, rs);
    return cl;
}

VAL idris_strlt(VM* vm, VAL l, VAL r) {
    char *ls = GETSTR(l);
    char *rs = GETSTR(r);

    return MKINT((i_int)(strcmp(ls, rs) < 0));
}

VAL idris_streq(VM* vm, VAL l, VAL r) {
    char *ls = GETSTR(l);
    char *rs = GETSTR(r);

    return MKINT((i_int)(strcmp(ls, rs) == 0));
}

VAL idris_strlen(VM* vm, VAL l) {
    return MKINT((i_int)(strlen(GETSTR(l))));
}

#define BUFSIZE 256

VAL idris_readStr(VM* vm, FILE* h) {
// Modified from 'safe-fgets.c' in the gdb distribution.
// (see http://www.gnu.org/software/gdb/current/)
    char *line_ptr;
    char* line_buf = (char *) malloc (BUFSIZE);
    int line_buf_size = BUFSIZE;

    /* points to last byte */
    line_ptr = line_buf + line_buf_size - 1;

    /* so we can see if fgets put a 0 there */
    *line_ptr = 1;
    if (fgets (line_buf, line_buf_size, h) == 0)
        return MKSTR(vm, "");

    /* we filled the buffer? */
    while (line_ptr[0] == 0 && line_ptr[-1] != '\n')
    {
        /* Make the buffer bigger and read more of the line */
        line_buf_size += BUFSIZE;
        line_buf = (char *) realloc (line_buf, line_buf_size);

        /* points to last byte again */
        line_ptr = line_buf + line_buf_size - 1;
        /* so we can see if fgets put a 0 there */
        *line_ptr = 1;

        if (fgets (line_buf + line_buf_size - BUFSIZE - 1, BUFSIZE + 1, h) == 0)
           return MKSTR(vm, "");
    }

    VAL str = MKSTR(vm, line_buf);
//    printf("DBG: %s\n", line_buf);
    free(line_buf);
    return str;
}

VAL idris_strHead(VM* vm, VAL str) {
    return MKINT((i_int)(GETSTR(str)[0]));
}

VAL MKSTROFFc(VM* vm, StrOffset* off) {
    Closure* cl = allocate(sizeof(Closure) + sizeof(StrOffset), 1);
    SETTY(cl, STROFFSET);
    cl->info.str_offset = (StrOffset*)((char*)cl + sizeof(Closure));

    cl->info.str_offset->str = off->str;
    cl->info.str_offset->offset = off->offset;

    return cl;
}

VAL idris_strTail(VM* vm, VAL str) {
    // If there's no room, just copy the string, or we'll have a problem after
    // gc moves str
    if (space(vm, sizeof(Closure) + sizeof(StrOffset))) {
        Closure* cl = allocate(sizeof(Closure) + sizeof(StrOffset), 0);
        SETTY(cl, STROFFSET);
        cl->info.str_offset = (StrOffset*)((char*)cl + sizeof(Closure));

        int offset = 0;
        VAL root = str;

        while(root!=NULL && !ISSTR(root)) { // find the root, carry on.
                              // In theory, at most one step here!
            offset += root->info.str_offset->offset;
            root = root->info.str_offset->str;
        }

        cl->info.str_offset->str = root;
        cl->info.str_offset->offset = offset+1;

        return cl;
    } else {
        return MKSTR(vm, GETSTR(str)+1);
    }
}

VAL idris_strCons(VM* vm, VAL x, VAL xs) {
    char *xstr = GETSTR(xs);
    Closure* cl = allocate(sizeof(Closure) +
                           strlen(xstr) + 2, 0);
    SETTY(cl, STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    cl -> info.str[0] = (char)(GETINT(x));
    strcpy(cl -> info.str+1, xstr);
    return cl;
}

VAL idris_strIndex(VM* vm, VAL str, VAL i) {
    return MKINT((i_int)(GETSTR(str)[GETINT(i)]));
}

VAL idris_strRev(VM* vm, VAL str) {
    char *xstr = GETSTR(str);
    Closure* cl = allocate(sizeof(Closure) +
                           strlen(xstr) + 1, 0);
    SETTY(cl, STRING);
    cl -> info.str = (char*)cl + sizeof(Closure);
    int y = 0;
    int x = strlen(xstr);

    cl-> info.str[x+1] = '\0';
    while(x>0) {
        cl -> info.str[y++] = xstr[--x];
    }
    return cl;
}

VAL idris_systemInfo(VM* vm, VAL index) {
    int i = GETINT(index);
    switch(i) {
        case 0: // backend
            return MKSTR(vm, "c");
        case 1:
            return MKSTR(vm, IDRIS_TARGET_OS);
        case 2:
            return MKSTR(vm, IDRIS_TARGET_TRIPLE);
    }
    return MKSTR(vm, "");
}

VAL MKBUFFERc(VM* vm, Buffer* buf) {
    Closure* cl = allocate(sizeof(Closure) + sizeof *buf + buf->cap, 1);
    SETTY(cl, BUFFER);
    cl->info.buf = (Buffer*)((void*)cl + sizeof(Closure));
    memmove(cl->info.buf, buf, sizeof *buf + buf->fill);
    return cl;
}

static VAL internal_allocate(VM *vm, size_t hint) {
    size_t size = hint + sizeof(Closure) + sizeof(Buffer);

    // Round up to a power of 2
    --size;
    size_t i;
    for (i = 0; i <= sizeof size; ++i)
        size |= size >> (1 << i);
    ++size;

    Closure* cl = allocate(size, 0);
    SETTY(cl, BUFFER);
    cl->info.buf = (Buffer*)((void*)cl + sizeof(Closure));
    cl->info.buf->cap = size - (sizeof(Closure) + sizeof(Buffer));
    return cl;
}

// Following functions cast uint64_t to size_t, which may narrow!
VAL idris_buffer_allocate(VM* vm, VAL hint) {
    Closure* cl = internal_allocate(vm, hint->info.bits64);
    cl->info.buf->fill = 0;
    return cl;
}

static void internal_memset(void *dest, const void *src, size_t size, size_t num) {
    while (num--) {
        memmove(dest, src, size);
        dest += size;
    }
}

static VAL internal_prepare_append(VM* vm, VAL buf, size_t bufLen, size_t appLen) {
    size_t totalLen = bufLen + appLen;
    Closure* cl;
    if (bufLen != buf->info.buf->fill ||
            totalLen > buf->info.buf->cap) {
        // We're not at the fill or are over cap, so need a new buffer
        cl = internal_allocate(vm, totalLen);
        memmove(cl->info.buf->store,
                buf->info.buf->store,
                bufLen);
        cl->info.buf->fill = totalLen;
    } else {
        // Hooray, can just bump the fill
        cl = buf;
        cl->info.buf->fill += appLen;
    }
    return cl;
}

VAL idris_appendBuffer(VM* vm, VAL fst, VAL fstLen, VAL cnt, VAL sndLen, VAL sndOff, VAL snd) {
    size_t firstLength = fstLen->info.bits64;
    size_t secondLength = sndLen->info.bits64;
    size_t count = cnt->info.bits64;
    size_t offset = sndOff->info.bits64;
    Closure* cl = internal_prepare_append(vm, fst, firstLength, count * secondLength);
    internal_memset(cl->info.buf->store + firstLength, snd->info.buf->store + offset, secondLength, count);
    return cl;
}

// Special cased because we can use memset
VAL idris_appendB8Native(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    size_t bufLen = len->info.bits64;
    size_t count = cnt->info.bits64;
    Closure* cl = internal_prepare_append(vm, buf, bufLen, count);
    memset(cl->info.buf->store + bufLen, val->info.bits8, count);
    return cl;
}

static VAL internal_append_bits(VM* vm, VAL buf, VAL bufLen, VAL cnt, const void* val, size_t val_len) {
    size_t len = bufLen->info.bits64;
    size_t count = cnt->info.bits64;
    Closure* cl = internal_prepare_append(vm, buf, len, count * val_len);
    internal_memset(cl->info.buf->store + len, val, val_len, count);
    return cl;
}

VAL idris_appendB16Native(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    return internal_append_bits(vm, buf, len, cnt, &val->info.bits16, sizeof val->info.bits16);
}

VAL idris_appendB16LE(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    // On gcc 4.8 -O3 compiling for x86_64, using leVal like this is
    // optimized away. Presumably the same holds for other sizes and
    // conversly on BE systems
    unsigned char leVal[sizeof val] = { val->info.bits16
                                      , (val->info.bits16 >> 8)
                                      };
    return internal_append_bits(vm, buf, len, cnt, leVal, sizeof leVal);
}

VAL idris_appendB16BE(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    unsigned char beVal[sizeof val] = { (val->info.bits16 >> 8)
                                      , val->info.bits16
                                      };
    return internal_append_bits(vm, buf, len, cnt, beVal, sizeof beVal);
}

VAL idris_appendB32Native(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    return internal_append_bits(vm, buf, len, cnt, &val->info.bits32, sizeof val->info.bits32);
}

VAL idris_appendB32LE(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    unsigned char leVal[sizeof val] = { val->info.bits32
                                      , (val->info.bits32 >> 8)
                                      , (val->info.bits32 >> 16)
                                      , (val->info.bits32 >> 24)
                                      };
    return internal_append_bits(vm, buf, len, cnt, leVal, sizeof leVal);
}

VAL idris_appendB32BE(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    unsigned char beVal[sizeof val] = { (val->info.bits32 >> 24)
                                      , (val->info.bits32 >> 16)
                                      , (val->info.bits32 >> 8)
                                      , val->info.bits32
                                      };
    return internal_append_bits(vm, buf, len, cnt, beVal, sizeof beVal);
}

VAL idris_appendB64Native(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    return internal_append_bits(vm, buf, len, cnt, &val->info.bits64, sizeof val->info.bits64);
}

VAL idris_appendB64LE(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    unsigned char leVal[sizeof val] = { val->info.bits64
                                      , (val->info.bits64 >> 8)
                                      , (val->info.bits64 >> 16)
                                      , (val->info.bits64 >> 24)
                                      , (val->info.bits64 >> 32)
                                      , (val->info.bits64 >> 40)
                                      , (val->info.bits64 >> 48)
                                      , (val->info.bits64 >> 56)
                                      };
    return internal_append_bits(vm, buf, len, cnt, leVal, sizeof leVal);
}

VAL idris_appendB64BE(VM* vm, VAL buf, VAL len, VAL cnt, VAL val) {
    unsigned char beVal[sizeof val] = { (val->info.bits64 >> 56)
                                      , (val->info.bits64 >> 48)
                                      , (val->info.bits64 >> 40)
                                      , (val->info.bits64 >> 32)
                                      , (val->info.bits64 >> 24)
                                      , (val->info.bits64 >> 16)
                                      , (val->info.bits64 >> 8)
                                      , val->info.bits64
                                      };
    return internal_append_bits(vm, buf, len, cnt, beVal, sizeof beVal);
}

VAL idris_peekB8Native(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    uint8_t *val = buf->info.buf->store + offset;
    return MKB8(vm, *val);
}

VAL idris_peekB16Native(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    uint16_t *val = (uint16_t *) (buf->info.buf->store + offset);
    return MKB16(vm, *val);
}

VAL idris_peekB16LE(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    return MKB16(vm, ((uint16_t) buf->info.buf->store[offset]) +
                     (((uint16_t) buf->info.buf->store[offset + 1]) << 8));
}

VAL idris_peekB16BE(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    return MKB16(vm, ((uint16_t) buf->info.buf->store[offset + 1]) +
                     (((uint16_t) buf->info.buf->store[offset]) << 8));
}

VAL idris_peekB32Native(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    uint32_t *val = (uint32_t *) (buf->info.buf->store + offset);
    return MKB32(vm, *val);
}

VAL idris_peekB32LE(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    return MKB32(vm, ((uint32_t) buf->info.buf->store[offset]) +
                     (((uint32_t) buf->info.buf->store[offset + 1]) << 8) +
                     (((uint32_t) buf->info.buf->store[offset + 2]) << 16) +
                     (((uint32_t) buf->info.buf->store[offset + 3]) << 24));
}

VAL idris_peekB32BE(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    return MKB32(vm, ((uint32_t) buf->info.buf->store[offset + 3]) +
                     (((uint32_t) buf->info.buf->store[offset + 2]) << 8) +
                     (((uint32_t) buf->info.buf->store[offset + 1]) << 16) +
                     (((uint32_t) buf->info.buf->store[offset]) << 24));
}

VAL idris_peekB64Native(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    uint64_t *val = (uint64_t *) (buf->info.buf->store + offset);
    return MKB64(vm, *val);
}

VAL idris_peekB64LE(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    return MKB64(vm, ((uint64_t) buf->info.buf->store[offset]) +
                     (((uint64_t) buf->info.buf->store[offset + 1]) << 8) +
                     (((uint64_t) buf->info.buf->store[offset + 2]) << 16) +
                     (((uint64_t) buf->info.buf->store[offset + 3]) << 24) +
                     (((uint64_t) buf->info.buf->store[offset + 4]) << 32) +
                     (((uint64_t) buf->info.buf->store[offset + 5]) << 40) +
                     (((uint64_t) buf->info.buf->store[offset + 6]) << 48) +
                     (((uint64_t) buf->info.buf->store[offset + 7]) << 56));
}

VAL idris_peekB64BE(VM* vm, VAL buf, VAL off) {
    size_t offset = off->info.bits64;
    return MKB64(vm, ((uint64_t) buf->info.buf->store[offset + 7]) +
                     (((uint64_t) buf->info.buf->store[offset + 6]) << 8) +
                     (((uint64_t) buf->info.buf->store[offset + 5]) << 16) +
                     (((uint64_t) buf->info.buf->store[offset + 4]) << 24) +
                     (((uint64_t) buf->info.buf->store[offset + 3]) << 32) +
                     (((uint64_t) buf->info.buf->store[offset + 2]) << 40) +
                     (((uint64_t) buf->info.buf->store[offset + 1]) << 48) +
                     (((uint64_t) buf->info.buf->store[offset]) << 56));
}

typedef struct {
    VM* vm; // thread's VM
    VM* callvm; // calling thread's VM
    func fn;
    VAL arg;
} ThreadData;

#ifdef HAS_PTHREAD
void* runThread(void* arg) {
    ThreadData* td = (ThreadData*)arg;
    VM* vm = td->vm;
    VM* callvm = td->callvm;

    init_threaddata(vm);

    TOP(0) = td->arg;
    BASETOP(0);
    ADDTOP(1);
    td->fn(vm, NULL);
    callvm->processes--;

    free(td);

    //    Stats stats =
    terminate(vm);
    //    aggregate_stats(&(td->vm->stats), &stats);
    return NULL;
}

void* vmThread(VM* callvm, func f, VAL arg) {
    VM* vm = init_vm(callvm->stack_max - callvm->valstack, callvm->heap.size,
                     callvm->max_threads);
    vm->processes=1; // since it can send and receive messages
    pthread_t t;
    pthread_attr_t attr;
//    size_t stacksize;

    pthread_attr_init(&attr);
//    pthread_attr_getstacksize (&attr, &stacksize);
//    pthread_attr_setstacksize (&attr, stacksize*64);

    ThreadData *td = malloc(sizeof(ThreadData));
    td->vm = vm;
    td->callvm = callvm;
    td->fn = f;
    td->arg = copyTo(vm, arg);

    callvm->processes++;

    pthread_create(&t, &attr, runThread, td);
//    usleep(100);
    return vm;
}

// VM is assumed to be a different vm from the one x lives on

VAL doCopyTo(VM* vm, VAL x) {
    int i, ar;
    VAL* argptr;
    Closure* cl;
    if (x==NULL || ISINT(x)) {
        return x;
    }
    switch(GETTY(x)) {
    case CON:
        ar = CARITY(x);
        if (ar == 0 && CTAG(x) < 256) { // globally allocated
            cl = x;
        } else {
            allocCon(cl, vm, CTAG(x), ar, 1);

            argptr = (VAL*)(cl->info.c.args);
            for(i = 0; i < ar; ++i) {
                *argptr = doCopyTo(vm, *((VAL*)(x->info.c.args)+i)); // recursive version
                argptr++;
            }
        }
        break;
    case FLOAT:
        cl = MKFLOATc(vm, x->info.f);
        break;
    case STRING:
        cl = MKSTRc(vm, x->info.str);
        break;
    case BUFFER:
        cl = MKBUFFERc(vm, x->info.buf);
        break;
    case BIGINT:
        cl = MKBIGMc(vm, x->info.ptr);
        break;
    case PTR:
        cl = MKPTRc(vm, x->info.ptr);
        break;
    case MANAGEDPTR:
        cl = MKMPTRc(vm, x->info.mptr->data, x->info.mptr->size);
        break;
    case BITS8:
        cl = idris_b8CopyForGC(vm, x);
        break;
    case BITS16:
        cl = idris_b16CopyForGC(vm, x);
        break;
    case BITS32:
        cl = idris_b32CopyForGC(vm, x);
        break;
    case BITS64:
        cl = idris_b64CopyForGC(vm, x);
        break;
    default:
        assert(0); // We're in trouble if this happens...
    }
    return cl;
}

VAL copyTo(VM* vm, VAL x) {
    VM* current = pthread_getspecific(vm_key);
    pthread_setspecific(vm_key, vm);
    VAL ret = doCopyTo(vm, x);
    pthread_setspecific(vm_key, current);
    return ret;
}

// Add a message to another VM's message queue
void idris_sendMessage(VM* sender, VM* dest, VAL msg) {
    // FIXME: If GC kicks in in the middle of the copy, we're in trouble.
    // Probably best check there is enough room in advance. (How?)

    // Also a problem if we're allocating at the same time as the
    // destination thread (which is very likely)
    // Should the inbox be a different memory space?

    // So: we try to copy, if a collection happens, we do the copy again
    // under the assumption there's enough space this time.

    int gcs = dest->stats.collections;
    pthread_mutex_lock(&dest->alloc_lock);
    VAL dmsg = copyTo(dest, msg);
    pthread_mutex_unlock(&dest->alloc_lock);

    if (dest->stats.collections > gcs) {
        // a collection will have invalidated the copy
        pthread_mutex_lock(&dest->alloc_lock);
        dmsg = copyTo(dest, msg); // try again now there's room...
        pthread_mutex_unlock(&dest->alloc_lock);
    }

    pthread_mutex_lock(&(dest->inbox_lock));
    
    if (dest->inbox_write >= dest->inbox_end) {
        // FIXME: This is obviously bad in the long run. This should
        // either block, make the inbox bigger, or return an error code,
        // or possibly make it user configurable
        fprintf(stderr, "Inbox full"); 
        exit(-1);
    }

    dest->inbox_write->msg = dmsg;
    dest->inbox_write->sender = sender;
    dest->inbox_write++;

    // Wake up the other thread
    pthread_mutex_lock(&dest->inbox_block);
    pthread_cond_signal(&dest->inbox_waiting);
    pthread_mutex_unlock(&dest->inbox_block);

//    printf("Sending [signalled]...\n");

    pthread_mutex_unlock(&(dest->inbox_lock));
//    printf("Sending [unlock]...\n");
}

VM* idris_checkMessages(VM* vm) {
    return idris_checkMessagesFrom(vm, NULL);
}

VM* idris_checkMessagesFrom(VM* vm, VM* sender) {
    Msg* msg;
    
    for (msg = vm->inbox; msg < vm->inbox_end && msg->msg != NULL; ++msg) {
        if (sender == NULL || msg->sender == sender) {
            return msg->sender;
        }
    }
    return 0;
}

Msg* idris_getMessageFrom(VM* vm, VM* sender) {
    Msg* msg;
    
    for (msg = vm->inbox; msg < vm->inbox_write; ++msg) {
        if (sender == NULL || msg->sender == sender) {
            return msg;
        }
    }
    return NULL;
}

// block until there is a message in the queue
Msg* idris_recvMessage(VM* vm) {
    return idris_recvMessageFrom(vm, NULL);
}

Msg* idris_recvMessageFrom(VM* vm, VM* sender) {
    Msg* msg;
    Msg* ret = malloc(sizeof(Msg));

    struct timespec timeout;
    int status;

    pthread_mutex_lock(&vm->inbox_block);
    msg = idris_getMessageFrom(vm, sender);

    while (msg == NULL) {
//        printf("No message yet\n");
//        printf("Waiting [lock]...\n");
        timeout.tv_sec = time (NULL) + 3;
        timeout.tv_nsec = 0;
        status = pthread_cond_timedwait(&vm->inbox_waiting, &vm->inbox_block,
                               &timeout);
        (void)(status); //don't emit 'unused' warning
//        printf("Waiting [unlock]... %d\n", status);
        msg = idris_getMessageFrom(vm, sender);
    }
    pthread_mutex_unlock(&vm->inbox_block);

    if (msg != NULL) {
        ret->msg = msg->msg;
        ret->sender = msg->sender;

        pthread_mutex_lock(&(vm->inbox_lock));

        // Slide everything down after the message in the inbox,
        // Move the inbox_write pointer down, and clear the value at the
        // end - O(n) but it's easier since the message from a specific
        // sender could be anywhere in the inbox

        for(;msg < vm->inbox_write; ++msg) {
            if (msg+1 != vm->inbox_end) {
                msg->sender = (msg + 1)->sender;
                msg->msg = (msg + 1)->msg;
            }
        }

        vm->inbox_write->msg = NULL;
        vm->inbox_write->sender = NULL;
        vm->inbox_write--;

        pthread_mutex_unlock(&(vm->inbox_lock));
    } else {
        fprintf(stderr, "No messages waiting");
        exit(-1);
    }

    return ret;
}
#endif

VAL idris_getMsg(Msg* msg) {
    return msg->msg;
}

VM* idris_getSender(Msg* msg) {
    return msg->sender;
}

void idris_freeMsg(Msg* msg) {
    free(msg);
}

VAL* nullary_cons;

void init_nullaries() {
    int i;
    VAL cl;
    nullary_cons = malloc(256 * sizeof(VAL));
    for(i = 0; i < 256; ++i) {
        cl = malloc(sizeof(Closure));
        SETTY(cl, CON);
        cl->info.c.tag_arity = i << 8;
        nullary_cons[i] = cl;
    }
}

void free_nullaries() {
    int i;
    for(i = 0; i < 256; ++i) {
        free(nullary_cons[i]);
    }
    free(nullary_cons);
}

int __idris_argc;
char **__idris_argv;

int idris_numArgs() {
    return __idris_argc;
}

const char* idris_getArg(int i) {
    return __idris_argv[i];
}

void stackOverflow() {
  fprintf(stderr, "Stack overflow");
  exit(-1);
}
