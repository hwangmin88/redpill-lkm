/**
 * This little (and dangerous) utility allows for replacement of any arbitrary kernel symbols with your own
 *
 * Since we're in the kernel we can do anything we want. This also included manipulating the actual code of functions
 * executed by the kernel. So, if someone calls printk() it normally goes to the correct place. However this place can
 * be override with a snippet of ASM which jumps to another place - a place we specify. It doesn't require a genius to
 * understand the power and implication of this ;)
 *
 * HOW IT WORKS?
 * See header file for example of usage. In short this code does the following:
 * 0. Kernel protects .text pages (r/o by default, so you don't override the code by accident): they need to be unlocked
 * 1. Find where our symbol-to-be-replaced is located
 * 2. Disable memory pages write protection (as symbols are in .text which is )
 * 4. Make memory page containing symbol-to-be-replaced r/w
 * 5. Generate jump code ASM containing the address of new symbol specified by the caller
 * 6. Mark the memory page from [4] r/o again
 * 7. [optional] Process is fully reversible
 *
 * SYSCALL SPECIAL CASE
 * There's also a variant made for syscalls specifically. It differs by the fact that override_symbol() makes
 * the original code unusable (as the first bytes are replaced with a jump) yet allows you to replace ANY function. The
 * overridden_syscall() in the other hand changes a pointer in the syscalls table. That way you CAN use the original
 * pointer to call back the original Linux syscall code you replaced. It works roughly like so:
 * 0. Kernel keeps syscalls table in .data section which is marked as r/o: it needs to be found & unlocked (#2-4 above)
 * 1. replace pointer in the table with custom one
 * 2. relock memory
 *
 * CALLING THE ORIGINAL CODE PROBLEM
 * When we wrote this code intially it was meant to be a temporary stop-gap until we have time to write a proper
 * rerouting using kernel's "insn" framework. However, this approach only looks simple on the surface. In theory we just
 * need to override the function preamble with a simple trampoline consisting of MOV+JMP to our new code. This is rather
 * simple and work well. However, attempting to call the original code without restoring the full function to its
 * original shape opens a huge can of worms with many edge-cases. Again, in *theory* we can simply grab the original
 * preamble, attach a JMP to the original function just after the preamble, and execute it.. it will work MOST of the
 * time, but:
 *  - we must ensure we round copied preamble to full instruction
 *  - trampoline must be automatically padded with NOP-sled
 *  - if function has ANY arguments (and most do) we need to take care of fixing the stack or saving preamble with all
 *    pushes?
 *  - overriden function may be shorter than trampoline (ok, we don't handle this even now, but it's unlikely)
 *  - and the biggest one: RIP. Some instructions are execution-point addressed (e.g. jump -5 bytes from here).
 *    Detecting that for the preamble and blocking override of such function (or EVEN fixing that RIP/EIP addressing) is
 *    rather possible. However, what becomes a problem are backward jumps in the code following the preamble. If the
 *    code after the preamble jumps backwards and lands in our trampoline instead of the original preamble it will
 *    either jump into a "random" place or jump to the replacement function. This is not that unlikely if the function
 *    is a short loop and nothing else. Trying to find such bug would be nightmare and we don't see a sane way of
 *    scanning the whole function and determining if it has any negative RIPs/EIPs and if they happen to fall within
 *    preamble. It's a mess with maaaany traps. While kernel has kprobes and fprobes we cannot use them as they're not
 *    enabled in syno kernels.
 *
 * We decided to compromise. The code offers a special call_overridden_symbol(). It follows a very similar process to
 * restore_symbol()+override_symbol(). Normally the restoration [R] + override [O] process chain will look like so:
 * 1.  [R] Disable CR0
 * 2.  [R] Unlock memory page(s) where trampoline lies
 * 3.  [R] Copy original preamble over trampoline
 * 4.  [R] Lock memory page(s) with preamble
 * 5.  [R] Enable CR0
 * 6.  Call original
 * 7.  [O] Disable CR0
 * 8.  [R] Unlock memory page(s) where we want to copy the trampoline
 * 9.  [R] Copy original trampoline over original preamble
 * 10. [R] Lock memory page(s) with trampoline
 * 11. [R] Enable CR0
 *
 * The call_overridden_symbol() obviously has to disable CR0 and unlock memory but it LEAVES the memory unlocked for any
 * subsequent calls. While it's less safe (as something can accidentally override it will not be reported) it shortens
 * the call path for 2nd and beyond calls to the original:
 * 1. Check if memory needs to be unlocked
 * 2. [O] Copy original preamble over trampoline
 * 3. Call original
 * 4. [R] Copy original trampoline over original preamble
 *
 * Using call_overridden_symbol() thus has huge advantages over override+restore if you plan to call the original
 * function more than once. If you want to call it only once the call_overridden_symbol() is an equivalent of restore+
 * override. That's why, for readability reasons and DRY of checking code it's preferred to use call_overridden_symbol()
 * even if you call the original method even once.
 * There's a third method: utilizing forceful breakpoints like kprobe does. However, this is a rather complex system and
 * also contains many traps. Additionally, its overhead is no smaller than the current call_overridden_symbol()
 * implementation. The kernel uses breakpoints for more safety and to detect possible interactions between different
 * subsystems utilizing breakpoints. This isn't our concern here.
 *
 * References:
 *  - https://www.cs.uaf.edu/2016/fall/cs301/lecture/09_28_machinecode.html
 *  - http://www.watson.org/%7Erobert/2007woot/2007usenixwoot-exploitingconcurrency.pdf
 *  - https://stackoverflow.com/a/5711253
 *  - https://www.kernel.org/doc/Documentation/kprobes.txt
 *  - https://stackoverflow.com/a/6742086
 */

#include "override_symbol.h"
#include "../common.h"
#include "call_protected.h" //_flush_tlb_all()
#include <asm/cacheflush.h> //PAGE_ALIGN
#include <asm/asm-offsets.h> //__NR_syscall_max & NR_syscalls
#include <asm/unistd_64.h> //syscalls numbers (e.g. __NR_read)
#include <linux/kallsyms.h> //kallsyms_lookup_name()
#include <linux/string.h> //memcpy()

#define JUMP_ADDR_POS 2 //JUMP starts at [2] in the jump template below
static const unsigned char jump_tpl[OVERRIDE_JUMP_SIZE] =
    "\x48\xb8" "\x00\x00\x00\x00\x00\x00\x00\x00" /* MOVQ 64-bit-vaddr, %rax */
    "\xff\xe0" /* JMP *%rax */
;

#define PAGE_ALIGN_BOTTOM(addr) (PAGE_ALIGN(addr) - PAGE_SIZE) //aligns the memory address to bottom of the page boundary
#define NUM_PAGES_BETWEEN(low, high) (((PAGE_ALIGN_BOTTOM(high) - PAGE_ALIGN_BOTTOM(low)) / PAGE_SIZE) + 1)

#define WITH_OVS_LOCK(__sym, code)                                     \
    do {                                                               \
        pr_loc_dbg("Obtaining lock for <%p>", __sym->org_sym_ptr);     \
        spin_lock_irqsave(&__sym->lock, __sym->lock_irq);              \
        ({code});                                                      \
        spin_unlock_irqrestore(&__sym->lock, __sym->lock_irq);         \
        pr_loc_dbg("Released lock for <%p>", __sym->org_sym_ptr);      \
    } while(0);

/**
 * Disables write-protection for the memory where symbol resides
 *
 * There are a million different methods of circumventing the memory protection in Linux. The reason being the kernel
 * people make it harder and harder to modify syscall table (& others in the process), which in general is a great idea.
 * There are two core methods people use: 1) disabling CR0 WP bit, and 2) setting memory page(s) as R/W.
 * The 1) is a flag, present on x86 CPUs, which when cleared configures the MMU to *ignore* write protection set on
 * memory regions. However, this flag is per-core (=synchronization problems) and it works as all-or-none. We don't
 * want to leave such a big thing disabled (especially for long time).
 * The second mechanism disabled memory protection on per-page basis. Normally the kernel contains set_memory_rw() which
 * does what it says - sets the address (which should be lower-page aligned) to R/W. However, this function is evil for
 * some time (~2.6?). In its course it calls static_protections() which REMOVES the R/W flag from the request
 * (effectively making the call a noop) while still returning 0. Guess how long we debugged that... additionally, that
 * function is removed in newer kernels.
 * The easiest way is to just lookup the page table entry for a given address, modify the R/W attribute directly and
 * dump CPU caches. This will work as there's no middle-man to mess with our request.
 */
static void set_mem_rw(const unsigned long vaddr, unsigned long len)
{
    unsigned long addr = PAGE_ALIGN_BOTTOM(vaddr);
    pr_loc_dbg("Disabling memory protection for page(s) at %p+%lu/%u (<<%p)", (void *) vaddr, len,
               (unsigned int) NUM_PAGES_BETWEEN(vaddr, vaddr + len), (void *) addr);

    //theoretically this should use set_pte_atomic() but we're touching pages that will not be modified by anything else
    unsigned int level;
    for(; addr <= vaddr; addr += PAGE_SIZE) {
        pte_t *pte = lookup_address(addr, &level);
        pte->pte |= _PAGE_RW;
    }

    _flush_tlb_all();
}

/**
 * Reverses set_mem_rw()
 *
 * See set_mem_rw() for details
 */
static void set_mem_ro(const unsigned long vaddr, unsigned long len)
{
    unsigned long addr = PAGE_ALIGN_BOTTOM(vaddr);
    pr_loc_dbg("Enabling memory protection for page(s) at %p+%lu/%u (<<%p)", (void *) vaddr, len,
               (unsigned int) NUM_PAGES_BETWEEN(vaddr, vaddr + len), (void *) addr);

    //theoretically this should use set_pte_atomic() but we're touching pages that will not be modified by anything else
    unsigned int level;
    for(; addr <= vaddr; addr += PAGE_SIZE) {
        pte_t *pte = lookup_address(addr, &level);
        pte->pte &= ~_PAGE_RW;
    }

    _flush_tlb_all();
}

int override_symbol(const char *name, const void *new_sym_ptr, void * *org_sym_ptr, unsigned char *org_sym_code)
{
    pr_loc_dbg("Overriding %s() with %pf()<%p>", name, new_sym_ptr, new_sym_ptr);

    *org_sym_ptr = (void *)kallsyms_lookup_name(name);
    if (*org_sym_ptr == 0) {
        pr_loc_err("Failed to locate vaddr for %s()", name);
        return -EFAULT;
    }
    pr_loc_dbg("Found %s() @ <%p>", name, *org_sym_ptr);

    //First generate jump new_sym_ptr
    unsigned char jump[OVERRIDE_JUMP_SIZE];
    memcpy(jump, jump_tpl, OVERRIDE_JUMP_SIZE);
    *(long *)&jump[JUMP_ADDR_POS] = (long)new_sym_ptr;
    pr_loc_dbg("Generated jump to f()<%p> for %s()<%p>: "
               "%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x",
               new_sym_ptr, name, *org_sym_ptr,
               jump[0],jump[1], jump[2],jump[3],jump[4],jump[5],jump[6],jump[7],jump[8],jump[9], jump[10],jump[11]);

    memcpy(org_sym_code, *org_sym_ptr, OVERRIDE_JUMP_SIZE); //Backup old code

    set_mem_rw((long)*org_sym_ptr, OVERRIDE_JUMP_SIZE);
    pr_loc_dbg("Writing jump code to <%p>", *org_sym_ptr);
    memcpy(*org_sym_ptr, jump, OVERRIDE_JUMP_SIZE);
    set_mem_ro((long)*org_sym_ptr, OVERRIDE_JUMP_SIZE);

    pr_loc_dbg("Override for %s set up with %p", name, new_sym_ptr);
    return 0;
}

struct override_symbol_inst {
    void *org_sym_ptr;
    const void *new_sym_ptr;
    char org_sym_code[OVERRIDE_JUMP_SIZE];
    char trampoline[OVERRIDE_JUMP_SIZE];
    spinlock_t lock;
    unsigned long lock_irq;
    bool installed:1; //whether the symbol is currently overrode (=has trampoline installed)
    bool has_trampoline:1; //does this structure contain a valid trampoline code already?
    bool mem_protected:1; //is the trampoline installation site memory-protected?
    char name[];
};

static inline void put_ov_symbol_instance(struct override_symbol_inst *sym)
{
    kfree(sym);
}

static struct override_symbol_inst* get_ov_symbol_instance(const char *symbol_name, const void *new_sym_ptr)
{
    struct override_symbol_inst *sym = kmalloc(
            sizeof(struct override_symbol_inst) + sizeof(char) * (strlen(symbol_name) + 1), GFP_KERNEL);
    if (unlikely(!sym)) {
        pr_loc_crt("kmalloc failed");
        return ERR_PTR(-ENOMEM);
    }

    sym->new_sym_ptr = new_sym_ptr;
    spin_lock_init(&sym->lock);
    sym->installed = false;
    sym->has_trampoline = false;
    sym->mem_protected = true;
    strcpy(sym->name, symbol_name);

    sym->org_sym_ptr = (void *)kallsyms_lookup_name(sym->name);
    if (unlikely(sym->org_sym_ptr == 0)) { //header file: "Lookup the address for a symbol. Returns 0 if not found."
        pr_loc_err("Failed to locate vaddr for %s()", sym->name);
        put_ov_symbol_instance(sym);
        return ERR_PTR(-EFAULT);
    }
    pr_loc_dbg("Saved %s() ptr <%p>", sym->name, sym->org_sym_ptr);

    return sym;
}

static void __always_inline enable_mem_protection(struct override_symbol_inst *sym)
{
    set_mem_rw((unsigned long)sym->org_sym_ptr, OVERRIDE_JUMP_SIZE);
    sym->mem_protected = true;
}

static void __always_inline disable_mem_protection(struct override_symbol_inst *sym)
{
    set_mem_rw((unsigned long)sym->org_sym_ptr, OVERRIDE_JUMP_SIZE);
    sym->mem_protected = false;
}

/**
 * Generates trampoline code to jump from old symbol to the new symbol location and saves the original code
 */
static inline void prepare_trampoline(struct override_symbol_inst *sym)
{
    pr_loc_dbg("Generating trampoline");

    //First generate jump/trampoline to new_sym_ptr
    memcpy(sym->trampoline, jump_tpl, OVERRIDE_JUMP_SIZE); //copy "empty" trampoline
    *(long *)&sym->trampoline[JUMP_ADDR_POS] = (long)sym->new_sym_ptr; //paste new addr into trampoline
    pr_loc_dbg("Generated trampoline to %pF<%p> for %s<%p>: ", sym->new_sym_ptr, sym->new_sym_ptr, sym->name,
               sym->org_sym_ptr);

    memcpy(sym->org_sym_code, sym->org_sym_ptr, OVERRIDE_JUMP_SIZE); //Backup old code
    sym->has_trampoline = true;
}

int __enable_symbol_override(struct override_symbol_inst *sym)
{
    if (unlikely(sym->installed))
        return 0; //noop but not an error

    if (!sym->has_trampoline)
        prepare_trampoline(sym);

    if (sym->mem_protected)
        disable_mem_protection(sym);

    WITH_OVS_LOCK(sym,
        pr_loc_dbg("Writing trampoline code to <%p>", sym->org_sym_ptr);
        memcpy(sym->org_sym_ptr, sym->trampoline, OVERRIDE_JUMP_SIZE);
        sym->installed = true;
    );

    return 0;
}

int __disable_symbol_override(struct override_symbol_inst *sym)
{
    if (unlikely(!sym->installed))
        return 0; //noop but not an error

    if (sym->mem_protected)
        disable_mem_protection(sym);

    WITH_OVS_LOCK(sym,
        pr_loc_dbg("Writing original code to <%p>", sym->org_sym_ptr);
        memcpy(sym->org_sym_ptr, sym->org_sym_code, OVERRIDE_JUMP_SIZE);
        sym->installed = false;
    );

    return 0;
}

__always_inline void * __get_org_ptr(struct override_symbol_inst *sym)
{
    return sym->org_sym_ptr;
}

__always_inline bool symbol_is_overridden(struct override_symbol_inst *sym)
{
    return likely(sym) && sym->installed;
}

struct override_symbol_inst* __must_check override_symbol_ng(const char *name, const void *new_sym_ptr)
{
    pr_loc_dbg("Overriding %s() with %pf()<%p>", name, new_sym_ptr, new_sym_ptr);

    int out;
    struct override_symbol_inst *sym = get_ov_symbol_instance(name, new_sym_ptr);
    if (unlikely(IS_ERR(sym)))
        return sym;

    if ((out = __enable_symbol_override(sym)) != 0)
        goto error_out;

    enable_mem_protection(sym); //by design standard override leaves the memory protected

    pr_loc_dbg("Successfully overrode %s with trampoline to %pF<%p>", sym->name, sym->new_sym_ptr, sym->new_sym_ptr);
    return sym;

    error_out:
    put_ov_symbol_instance(sym);
    return ERR_PTR(out);
}

int restore_symbol_ng(struct override_symbol_inst *sym)
{
    pr_loc_dbg("Restoring %s<%p> to original code", sym->name, sym->org_sym_ptr);

    int out;
    if ((out = __disable_symbol_override(sym)) != 0)
        goto out_free;

    enable_mem_protection(sym); //by design restore leaves the memory protected

    pr_loc_dbg("Successfully restored original code of %s", sym->name);

    out_free:
    put_ov_symbol_instance(sym);
    return out;
}

int restore_symbol(void * org_sym_ptr, const unsigned char *org_sym_code)
{
    pr_loc_dbg("Restoring symbol @ %pf()<%p>", org_sym_ptr, org_sym_ptr);

    set_mem_rw((long)org_sym_ptr, OVERRIDE_JUMP_SIZE);
    pr_loc_dbg("Writing original code to <%p>", org_sym_ptr);
    memcpy(org_sym_ptr, org_sym_code, OVERRIDE_JUMP_SIZE);
    set_mem_ro((long)org_sym_ptr, OVERRIDE_JUMP_SIZE);
    pr_loc_dbg("Symbol restored @ %pf()<%p>", org_sym_ptr, org_sym_ptr);

    return 0;
}

static unsigned long *syscall_table_ptr = NULL;
static void print_syscall_table(unsigned int from, unsigned to)
{
    if (unlikely(!syscall_table_ptr)) {
        pr_loc_dbg("Cannot print - no syscall_table_ptr address");
        return;
    }

    if (unlikely(from < 0 || to > __NR_syscall_max || from > to)) {
        pr_loc_bug("%s called with from=%d to=%d which are invalid", __FUNCTION__, from, to);
        return;
    }

    pr_loc_dbg("Printing syscall table %d-%d @ %p containing %d elements", from, to, (void *)syscall_table_ptr, NR_syscalls);
    for (unsigned int i = from; i < to; i++) {
        pr_loc_dbg("#%03d\t%pS", i, (void *)syscall_table_ptr[i]);
    }
}

static int find_sys_call_table(void)
{
    syscall_table_ptr = (unsigned long *)kallsyms_lookup_name("sys_call_table");
    if (syscall_table_ptr != 0) {
        pr_loc_dbg("Found sys_call_table @ <%p> using kallsyms", syscall_table_ptr);
        return 0;
    }

    //See https://kernelnewbies.kernelnewbies.narkive.com/L1uH0n8P/
    //In essence some systems will have it and some will not - finding it using kallsyms is the easiest and fastest
    pr_loc_dbg("Failed to locate vaddr for sys_call_table using kallsyms - falling back to memory search");

    /*
     There's also the bruteforce way - scan through the memory until you find it :D
     We know numbers for syscalls (e.g. __NR_close, __NR_write, __NR_read, etc.) which are essentially fixed positions
     in the sys_call_table. We also know addresses of functions handling these calls (sys_close/sys_write/sys_read
     etc.). This lets us scan the memory for one syscall address reference and when found confirm if this is really
     a place of sys_call_table by verifying other 2-3 places to make sure other syscalls are where they should be
     The huge downside of this method is it is slow as potentially the amount of memory to search may be large.
    */
    unsigned long sys_close_ptr = kallsyms_lookup_name("sys_close");
    unsigned long sys_open_ptr = kallsyms_lookup_name("sys_open");
    unsigned long sys_read_ptr = kallsyms_lookup_name("sys_read");
    unsigned long sys_write_ptr = kallsyms_lookup_name("sys_write");
    if (sys_close_ptr == 0 || sys_open_ptr == 0 || sys_read_ptr == 0 || sys_write_ptr == 0) {
        pr_loc_bug(
                "One or more syscall handler addresses cannot be located: "
                "sys_close<%p>, sys_open<%p>, sys_read<%p>, sys_write<%p>",
                (void *)sys_close_ptr, (void *)sys_open_ptr, (void *)sys_read_ptr, (void *)sys_write_ptr);
         return -EFAULT;
    }

    /*
     To speed up things a bit we search from a known syscall which was loaded early into the memory. To be safe we pick
     the earliest address and go from there. It can be nicely visualized on a system which DO export sys_call_table
     by running grep -E ' (__x64_)?sys_(close|open|read|write|call_table)$' /proc/kallsyms | sort
     You will get something like that:
      ffffffff860c18b0 T __x64_sys_close
      ffffffff860c37a0 T __x64_sys_open
      ffffffff860c7a80 T __x64_sys_read
      ffffffff860c7ba0 T __x64_sys_write
      ffffffff86e013a0 R sys_call_table    <= it's way below any of the syscalls but not too far (~13,892,336 bytes)
    */
    unsigned long i = sys_close_ptr;
    if (sys_open_ptr < i) i = sys_open_ptr;
    if (sys_read_ptr < i) i = sys_read_ptr;
    if (sys_write_ptr < i) i = sys_write_ptr;

    //If everything goes well it should take ~1-2ms tops (which is slow in the kernel sense but it's not bad)
    pr_loc_dbg("Scanning memory for sys_call_table starting at %p", (void *)i);
    for (; i < ULONG_MAX; i += sizeof(void *)) {
        syscall_table_ptr = (unsigned long *)i;

        if (unlikely(
                syscall_table_ptr[__NR_close] == sys_close_ptr &&
                syscall_table_ptr[__NR_open] == sys_open_ptr &&
                syscall_table_ptr[__NR_read] == sys_read_ptr &&
                syscall_table_ptr[__NR_write] == sys_write_ptr
        )) {
            pr_loc_dbg("Found sys_call_table @ %p", (void *)syscall_table_ptr);
            return 0;
        }
    }

    pr_loc_bug("Failed to find sys call table");
    syscall_table_ptr = NULL;
    return -EFAULT;
}

static unsigned long *overridden_syscall[NR_syscalls] = { NULL };
int override_syscall(unsigned int syscall_num, const void *new_sysc_ptr, void * *org_sysc_ptr)
{
    pr_loc_dbg("Overriding syscall #%d with %pf()<%p>", syscall_num, new_sysc_ptr, new_sysc_ptr);

    int out = 0;
    if (unlikely(!syscall_table_ptr)) {
        out = find_sys_call_table();
        if (unlikely(out != 0))
            return out;
    }

    if (unlikely(syscall_num > __NR_syscall_max)) {
        pr_loc_bug("Invalid syscall number: %d > %d", syscall_num, __NR_syscall_max);
        return -EINVAL;
    }

    print_syscall_table(syscall_num-5, syscall_num+5);

    if (unlikely(overridden_syscall[syscall_num])) {
        pr_loc_bug("Syscall %d is already overridden - will be replaced (bug?)", syscall_num);
    } else {
        //Only save original-original entry (not the override one)
        overridden_syscall[syscall_num] = (unsigned long *)syscall_table_ptr[syscall_num];
    }

    if (org_sysc_ptr != 0)
        *org_sysc_ptr = overridden_syscall[syscall_num];

    set_mem_rw((long)&syscall_table_ptr[syscall_num], sizeof(unsigned long));
    pr_loc_dbg("syscall #%d originally %ps<%p> will now be %ps<%p> @ %d", syscall_num,
               (void *) overridden_syscall[syscall_num], (void *) overridden_syscall[syscall_num], new_sysc_ptr,
               new_sysc_ptr, smp_processor_id());
    syscall_table_ptr[syscall_num] = (unsigned long) new_sysc_ptr;
    set_mem_ro((long)&syscall_table_ptr[syscall_num], sizeof(unsigned long));

    print_syscall_table(syscall_num-5, syscall_num+5);

    return out;
}

int restore_syscall(unsigned int syscall_num)
{
    pr_loc_dbg("Restoring syscall #%d", syscall_num);

    if (unlikely(!syscall_table_ptr)) {
        pr_loc_bug("Syscall table not found in %s ?!", __FUNCTION__);
        return -EFAULT;
    }

    if (unlikely(syscall_num > __NR_syscall_max)) {
        pr_loc_bug("Invalid syscall number: %d > %d", syscall_num, __NR_syscall_max);
        return -EINVAL;
    }

    if (unlikely(overridden_syscall[syscall_num] == 0)) {
        pr_loc_bug("Syscall #%d cannot be restored - it was never overridden", syscall_num);
        return -EINVAL;
    }

    print_syscall_table(syscall_num-5, syscall_num+5);

    set_mem_rw((long)&syscall_table_ptr[syscall_num], sizeof(unsigned long));
    pr_loc_dbg("Restoring syscall #%d from %ps<%p> to original %ps<%p>", syscall_num,
               (void *) syscall_table_ptr[syscall_num], (void *) syscall_table_ptr[syscall_num],
               (void *) overridden_syscall[syscall_num], (void *) overridden_syscall[syscall_num]);
    syscall_table_ptr[syscall_num] = (unsigned long)overridden_syscall[syscall_num];
    set_mem_rw((long)&syscall_table_ptr[syscall_num], sizeof(unsigned long));

    print_syscall_table(syscall_num-5, syscall_num+5);

    return 0;
}
