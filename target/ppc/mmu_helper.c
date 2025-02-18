/*
 *  PowerPC MMU, TLB, SLB and BAT emulation helpers for QEMU.
 *
 *  Copyright (c) 2003-2007 Jocelyn Mayer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "cpu.h"
#include "sysemu/kvm.h"
#include "kvm_ppc.h"
#include "mmu-hash64.h"
#include "mmu-hash32.h"
#include "exec/exec-all.h"
#include "exec/log.h"
#include "helper_regs.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/qemu-print.h"
#include "internal.h"
#include "mmu-book3s-v3.h"
#include "mmu-radix64.h"
#include "exec/helper-proto.h"
#include "exec/cpu_ldst.h"

/* #define DEBUG_BATS */
/* #define DEBUG_SOFTWARE_TLB */
/* #define DUMP_PAGE_TABLES */
/* #define FLUSH_ALL_TLBS */

#ifdef DEBUG_SOFTWARE_TLB
#  define LOG_SWTLB(...) qemu_log_mask(CPU_LOG_MMU, __VA_ARGS__)
#else
#  define LOG_SWTLB(...) do { } while (0)
#endif

#ifdef DEBUG_BATS
#  define LOG_BATS(...) qemu_log_mask(CPU_LOG_MMU, __VA_ARGS__)
#else
#  define LOG_BATS(...) do { } while (0)
#endif

/*****************************************************************************/
/* PowerPC MMU emulation */

/* Software driven TLB helpers */
static inline void ppc6xx_tlb_invalidate_all(CPUPPCState *env)
{
    ppc6xx_tlb_t *tlb;
    int nr, max;

    /* LOG_SWTLB("Invalidate all TLBs\n"); */
    /* Invalidate all defined software TLB */
    max = env->nb_tlb;
    if (env->id_tlbs == 1) {
        max *= 2;
    }
    for (nr = 0; nr < max; nr++) {
        tlb = &env->tlb.tlb6[nr];
        pte_invalidate(&tlb->pte0);
    }
    tlb_flush(env_cpu(env));
}

static inline void ppc6xx_tlb_invalidate_virt2(CPUPPCState *env,
                                               target_ulong eaddr,
                                               int is_code, int match_epn)
{
#if !defined(FLUSH_ALL_TLBS)
    CPUState *cs = env_cpu(env);
    ppc6xx_tlb_t *tlb;
    int way, nr;

    /* Invalidate ITLB + DTLB, all ways */
    for (way = 0; way < env->nb_ways; way++) {
        nr = ppc6xx_tlb_getnum(env, eaddr, way, is_code);
        tlb = &env->tlb.tlb6[nr];
        if (pte_is_valid(tlb->pte0) && (match_epn == 0 || eaddr == tlb->EPN)) {
            LOG_SWTLB("TLB invalidate %d/%d " TARGET_FMT_lx "\n", nr,
                      env->nb_tlb, eaddr);
            pte_invalidate(&tlb->pte0);
            tlb_flush_page(cs, tlb->EPN);
        }
    }
#else
    /* XXX: PowerPC specification say this is valid as well */
    ppc6xx_tlb_invalidate_all(env);
#endif
}

static inline void ppc6xx_tlb_invalidate_virt(CPUPPCState *env,
                                              target_ulong eaddr, int is_code)
{
    ppc6xx_tlb_invalidate_virt2(env, eaddr, is_code, 0);
}

static void ppc6xx_tlb_store(CPUPPCState *env, target_ulong EPN, int way,
                             int is_code, target_ulong pte0, target_ulong pte1)
{
    ppc6xx_tlb_t *tlb;
    int nr;

    nr = ppc6xx_tlb_getnum(env, EPN, way, is_code);
    tlb = &env->tlb.tlb6[nr];
    LOG_SWTLB("Set TLB %d/%d EPN " TARGET_FMT_lx " PTE0 " TARGET_FMT_lx
              " PTE1 " TARGET_FMT_lx "\n", nr, env->nb_tlb, EPN, pte0, pte1);
    /* Invalidate any pending reference in QEMU for this virtual address */
    ppc6xx_tlb_invalidate_virt2(env, EPN, is_code, 1);
    tlb->pte0 = pte0;
    tlb->pte1 = pte1;
    tlb->EPN = EPN;
    /* Store last way for LRU mechanism */
    env->last_way = way;
}

/* Generic TLB search function for PowerPC embedded implementations */
static int ppcemb_tlb_search(CPUPPCState *env, target_ulong address,
                             uint32_t pid)
{
    ppcemb_tlb_t *tlb;
    hwaddr raddr;
    int i, ret;

    /* Default return value is no match */
    ret = -1;
    for (i = 0; i < env->nb_tlb; i++) {
        tlb = &env->tlb.tlbe[i];
        if (ppcemb_tlb_check(env, tlb, &raddr, address, pid, 0, i) == 0) {
            ret = i;
            break;
        }
    }

    return ret;
}

/* Helpers specific to PowerPC 40x implementations */
static inline void ppc4xx_tlb_invalidate_all(CPUPPCState *env)
{
    ppcemb_tlb_t *tlb;
    int i;

    for (i = 0; i < env->nb_tlb; i++) {
        tlb = &env->tlb.tlbe[i];
        tlb->prot &= ~PAGE_VALID;
    }
    tlb_flush(env_cpu(env));
}

static void booke206_flush_tlb(CPUPPCState *env, int flags,
                               const int check_iprot)
{
    int tlb_size;
    int i, j;
    ppcmas_tlb_t *tlb = env->tlb.tlbm;

    for (i = 0; i < BOOKE206_MAX_TLBN; i++) {
        if (flags & (1 << i)) {
            tlb_size = booke206_tlb_size(env, i);
            for (j = 0; j < tlb_size; j++) {
                if (!check_iprot || !(tlb[j].mas1 & MAS1_IPROT)) {
                    tlb[j].mas1 &= ~MAS1_VALID;
                }
            }
        }
        tlb += booke206_tlb_size(env, i);
    }

    tlb_flush(env_cpu(env));
}

static int get_physical_address(CPUPPCState *env, mmu_ctx_t *ctx,
                                target_ulong eaddr, MMUAccessType access_type,
                                int type)
{
    return get_physical_address_wtlb(env, ctx, eaddr, access_type, type, 0);
}



/*****************************************************************************/
/* BATs management */
#if !defined(FLUSH_ALL_TLBS)
static inline void do_invalidate_BAT(CPUPPCState *env, target_ulong BATu,
                                     target_ulong mask)
{
    CPUState *cs = env_cpu(env);
    target_ulong base, end, page;

    base = BATu & ~0x0001FFFF;
    end = base + mask + 0x00020000;
    if (((end - base) >> TARGET_PAGE_BITS) > 1024) {
        /* Flushing 1024 4K pages is slower than a complete flush */
        LOG_BATS("Flush all BATs\n");
        tlb_flush(cs);
        LOG_BATS("Flush done\n");
        return;
    }
    LOG_BATS("Flush BAT from " TARGET_FMT_lx " to " TARGET_FMT_lx " ("
             TARGET_FMT_lx ")\n", base, end, mask);
    for (page = base; page != end; page += TARGET_PAGE_SIZE) {
        tlb_flush_page(cs, page);
    }
    LOG_BATS("Flush done\n");
}
#endif

static inline void dump_store_bat(CPUPPCState *env, char ID, int ul, int nr,
                                  target_ulong value)
{
    LOG_BATS("Set %cBAT%d%c to " TARGET_FMT_lx " (" TARGET_FMT_lx ")\n", ID,
             nr, ul == 0 ? 'u' : 'l', value, env->nip);
}

void helper_store_ibatu(CPUPPCState *env, uint32_t nr, target_ulong value)
{
    target_ulong mask;

    dump_store_bat(env, 'I', 0, nr, value);
    if (env->IBAT[0][nr] != value) {
        mask = (value << 15) & 0x0FFE0000UL;
#if !defined(FLUSH_ALL_TLBS)
        do_invalidate_BAT(env, env->IBAT[0][nr], mask);
#endif
        /*
         * When storing valid upper BAT, mask BEPI and BRPN and
         * invalidate all TLBs covered by this BAT
         */
        mask = (value << 15) & 0x0FFE0000UL;
        env->IBAT[0][nr] = (value & 0x00001FFFUL) |
            (value & ~0x0001FFFFUL & ~mask);
        env->IBAT[1][nr] = (env->IBAT[1][nr] & 0x0000007B) |
            (env->IBAT[1][nr] & ~0x0001FFFF & ~mask);
#if !defined(FLUSH_ALL_TLBS)
        do_invalidate_BAT(env, env->IBAT[0][nr], mask);
#else
        tlb_flush(env_cpu(env));
#endif
    }
}

void helper_store_ibatl(CPUPPCState *env, uint32_t nr, target_ulong value)
{
    dump_store_bat(env, 'I', 1, nr, value);
    env->IBAT[1][nr] = value;
}

void helper_store_dbatu(CPUPPCState *env, uint32_t nr, target_ulong value)
{
    target_ulong mask;

    dump_store_bat(env, 'D', 0, nr, value);
    if (env->DBAT[0][nr] != value) {
        /*
         * When storing valid upper BAT, mask BEPI and BRPN and
         * invalidate all TLBs covered by this BAT
         */
        mask = (value << 15) & 0x0FFE0000UL;
#if !defined(FLUSH_ALL_TLBS)
        do_invalidate_BAT(env, env->DBAT[0][nr], mask);
#endif
        mask = (value << 15) & 0x0FFE0000UL;
        env->DBAT[0][nr] = (value & 0x00001FFFUL) |
            (value & ~0x0001FFFFUL & ~mask);
        env->DBAT[1][nr] = (env->DBAT[1][nr] & 0x0000007B) |
            (env->DBAT[1][nr] & ~0x0001FFFF & ~mask);
#if !defined(FLUSH_ALL_TLBS)
        do_invalidate_BAT(env, env->DBAT[0][nr], mask);
#else
        tlb_flush(env_cpu(env));
#endif
    }
}

void helper_store_dbatl(CPUPPCState *env, uint32_t nr, target_ulong value)
{
    dump_store_bat(env, 'D', 1, nr, value);
    env->DBAT[1][nr] = value;
}

void helper_store_601_batu(CPUPPCState *env, uint32_t nr, target_ulong value)
{
    target_ulong mask;
#if defined(FLUSH_ALL_TLBS)
    int do_inval;
#endif

    dump_store_bat(env, 'I', 0, nr, value);
    if (env->IBAT[0][nr] != value) {
#if defined(FLUSH_ALL_TLBS)
        do_inval = 0;
#endif
        mask = (env->IBAT[1][nr] << 17) & 0x0FFE0000UL;
        if (env->IBAT[1][nr] & 0x40) {
            /* Invalidate BAT only if it is valid */
#if !defined(FLUSH_ALL_TLBS)
            do_invalidate_BAT(env, env->IBAT[0][nr], mask);
#else
            do_inval = 1;
#endif
        }
        /*
         * When storing valid upper BAT, mask BEPI and BRPN and
         * invalidate all TLBs covered by this BAT
         */
        env->IBAT[0][nr] = (value & 0x00001FFFUL) |
            (value & ~0x0001FFFFUL & ~mask);
        env->DBAT[0][nr] = env->IBAT[0][nr];
        if (env->IBAT[1][nr] & 0x40) {
#if !defined(FLUSH_ALL_TLBS)
            do_invalidate_BAT(env, env->IBAT[0][nr], mask);
#else
            do_inval = 1;
#endif
        }
#if defined(FLUSH_ALL_TLBS)
        if (do_inval) {
            tlb_flush(env_cpu(env));
        }
#endif
    }
}

void helper_store_601_batl(CPUPPCState *env, uint32_t nr, target_ulong value)
{
#if !defined(FLUSH_ALL_TLBS)
    target_ulong mask;
#else
    int do_inval;
#endif

    dump_store_bat(env, 'I', 1, nr, value);
    if (env->IBAT[1][nr] != value) {
#if defined(FLUSH_ALL_TLBS)
        do_inval = 0;
#endif
        if (env->IBAT[1][nr] & 0x40) {
#if !defined(FLUSH_ALL_TLBS)
            mask = (env->IBAT[1][nr] << 17) & 0x0FFE0000UL;
            do_invalidate_BAT(env, env->IBAT[0][nr], mask);
#else
            do_inval = 1;
#endif
        }
        if (value & 0x40) {
#if !defined(FLUSH_ALL_TLBS)
            mask = (value << 17) & 0x0FFE0000UL;
            do_invalidate_BAT(env, env->IBAT[0][nr], mask);
#else
            do_inval = 1;
#endif
        }
        env->IBAT[1][nr] = value;
        env->DBAT[1][nr] = value;
#if defined(FLUSH_ALL_TLBS)
        if (do_inval) {
            tlb_flush(env_cpu(env));
        }
#endif
    }
}

/*****************************************************************************/
/* TLB management */
void ppc_tlb_invalidate_all(CPUPPCState *env)
{
#if defined(TARGET_PPC64)
    if (mmu_is_64bit(env->mmu_model)) {
        env->tlb_need_flush = 0;
        tlb_flush(env_cpu(env));
    } else
#endif /* defined(TARGET_PPC64) */
    switch (env->mmu_model) {
    case POWERPC_MMU_SOFT_6xx:
    case POWERPC_MMU_SOFT_74xx:
        ppc6xx_tlb_invalidate_all(env);
        break;
    case POWERPC_MMU_SOFT_4xx:
    case POWERPC_MMU_SOFT_4xx_Z:
        ppc4xx_tlb_invalidate_all(env);
        break;
    case POWERPC_MMU_REAL:
        cpu_abort(env_cpu(env), "No TLB for PowerPC 4xx in real mode\n");
        break;
    case POWERPC_MMU_MPC8xx:
        /* XXX: TODO */
        cpu_abort(env_cpu(env), "MPC8xx MMU model is not implemented\n");
        break;
    case POWERPC_MMU_BOOKE:
    case POWERPC_MMU_476FP:
        tlb_flush(env_cpu(env));
        break;
    case POWERPC_MMU_BOOKE206:
        booke206_flush_tlb(env, -1, 0);
        break;
    case POWERPC_MMU_32B:
    case POWERPC_MMU_601:
        env->tlb_need_flush = 0;
        tlb_flush(env_cpu(env));
        break;
    default:
        /* XXX: TODO */
        cpu_abort(env_cpu(env), "Unknown MMU model %x\n", env->mmu_model);
        break;
    }
}

void ppc_tlb_invalidate_one(CPUPPCState *env, target_ulong addr)
{
#if !defined(FLUSH_ALL_TLBS)
    addr &= TARGET_PAGE_MASK;
#if defined(TARGET_PPC64)
    if (mmu_is_64bit(env->mmu_model)) {
        /* tlbie invalidate TLBs for all segments */
        /*
         * XXX: given the fact that there are too many segments to invalidate,
         *      and we still don't have a tlb_flush_mask(env, n, mask) in QEMU,
         *      we just invalidate all TLBs
         */
        env->tlb_need_flush |= TLB_NEED_LOCAL_FLUSH;
    } else
#endif /* defined(TARGET_PPC64) */
    switch (env->mmu_model) {
    case POWERPC_MMU_SOFT_6xx:
    case POWERPC_MMU_SOFT_74xx:
        ppc6xx_tlb_invalidate_virt(env, addr, 0);
        if (env->id_tlbs == 1) {
            ppc6xx_tlb_invalidate_virt(env, addr, 1);
        }
        break;
    case POWERPC_MMU_32B:
    case POWERPC_MMU_601:
        /*
         * Actual CPUs invalidate entire congruence classes based on
         * the geometry of their TLBs and some OSes take that into
         * account, we just mark the TLB to be flushed later (context
         * synchronizing event or sync instruction on 32-bit).
         */
        env->tlb_need_flush |= TLB_NEED_LOCAL_FLUSH;
        break;
    default:
        /* Should never reach here with other MMU models */
        assert(0);
    }
#else
    ppc_tlb_invalidate_all(env);
#endif
}

/*****************************************************************************/
/* Special registers manipulation */

/* Segment registers load and store */
target_ulong helper_load_sr(CPUPPCState *env, target_ulong sr_num)
{
#if defined(TARGET_PPC64)
    if (mmu_is_64bit(env->mmu_model)) {
        /* XXX */
        return 0;
    }
#endif
    return env->sr[sr_num];
}

void helper_store_sr(CPUPPCState *env, target_ulong srnum, target_ulong value)
{
    qemu_log_mask(CPU_LOG_MMU,
            "%s: reg=%d " TARGET_FMT_lx " " TARGET_FMT_lx "\n", __func__,
            (int)srnum, value, env->sr[srnum]);
#if defined(TARGET_PPC64)
    if (mmu_is_64bit(env->mmu_model)) {
        PowerPCCPU *cpu = env_archcpu(env);
        uint64_t esid, vsid;

        /* ESID = srnum */
        esid = ((uint64_t)(srnum & 0xf) << 28) | SLB_ESID_V;

        /* VSID = VSID */
        vsid = (value & 0xfffffff) << 12;
        /* flags = flags */
        vsid |= ((value >> 27) & 0xf) << 8;

        ppc_store_slb(cpu, srnum, esid, vsid);
    } else
#endif
    if (env->sr[srnum] != value) {
        env->sr[srnum] = value;
        /*
         * Invalidating 256MB of virtual memory in 4kB pages is way
         * longer than flushing the whole TLB.
         */
#if !defined(FLUSH_ALL_TLBS) && 0
        {
            target_ulong page, end;
            /* Invalidate 256 MB of virtual memory */
            page = (16 << 20) * srnum;
            end = page + (16 << 20);
            for (; page != end; page += TARGET_PAGE_SIZE) {
                tlb_flush_page(env_cpu(env), page);
            }
        }
#else
        env->tlb_need_flush |= TLB_NEED_LOCAL_FLUSH;
#endif
    }
}

/* TLB management */
void helper_tlbia(CPUPPCState *env)
{
    ppc_tlb_invalidate_all(env);
}

void helper_tlbie(CPUPPCState *env, target_ulong addr)
{
    ppc_tlb_invalidate_one(env, addr);
}

void helper_tlbiva(CPUPPCState *env, target_ulong addr)
{
    /* tlbiva instruction only exists on BookE */
    assert(env->mmu_model == POWERPC_MMU_BOOKE);
    assert(env->mmu_model == POWERPC_MMU_476FP);
    /* XXX: TODO */
    cpu_abort(env_cpu(env), "BookE MMU model is not implemented\n");
}

/* Software driven TLBs management */
/* PowerPC 602/603 software TLB load instructions helpers */
static void do_6xx_tlb(CPUPPCState *env, target_ulong new_EPN, int is_code)
{
    target_ulong RPN, CMP, EPN;
    int way;

    RPN = env->spr[SPR_RPA];
    if (is_code) {
        CMP = env->spr[SPR_ICMP];
        EPN = env->spr[SPR_IMISS];
    } else {
        CMP = env->spr[SPR_DCMP];
        EPN = env->spr[SPR_DMISS];
    }
    way = (env->spr[SPR_SRR1] >> 17) & 1;
    (void)EPN; /* avoid a compiler warning */
    LOG_SWTLB("%s: EPN " TARGET_FMT_lx " " TARGET_FMT_lx " PTE0 " TARGET_FMT_lx
              " PTE1 " TARGET_FMT_lx " way %d\n", __func__, new_EPN, EPN, CMP,
              RPN, way);
    /* Store this TLB */
    ppc6xx_tlb_store(env, (uint32_t)(new_EPN & TARGET_PAGE_MASK),
                     way, is_code, CMP, RPN);
}

void helper_6xx_tlbd(CPUPPCState *env, target_ulong EPN)
{
    do_6xx_tlb(env, EPN, 0);
}

void helper_6xx_tlbi(CPUPPCState *env, target_ulong EPN)
{
    do_6xx_tlb(env, EPN, 1);
}

/* PowerPC 74xx software TLB load instructions helpers */
static void do_74xx_tlb(CPUPPCState *env, target_ulong new_EPN, int is_code)
{
    target_ulong RPN, CMP, EPN;
    int way;

    RPN = env->spr[SPR_PTELO];
    CMP = env->spr[SPR_PTEHI];
    EPN = env->spr[SPR_TLBMISS] & ~0x3;
    way = env->spr[SPR_TLBMISS] & 0x3;
    (void)EPN; /* avoid a compiler warning */
    LOG_SWTLB("%s: EPN " TARGET_FMT_lx " " TARGET_FMT_lx " PTE0 " TARGET_FMT_lx
              " PTE1 " TARGET_FMT_lx " way %d\n", __func__, new_EPN, EPN, CMP,
              RPN, way);
    /* Store this TLB */
    ppc6xx_tlb_store(env, (uint32_t)(new_EPN & TARGET_PAGE_MASK),
                     way, is_code, CMP, RPN);
}

void helper_74xx_tlbd(CPUPPCState *env, target_ulong EPN)
{
    do_74xx_tlb(env, EPN, 0);
}

void helper_74xx_tlbi(CPUPPCState *env, target_ulong EPN)
{
    do_74xx_tlb(env, EPN, 1);
}

/*****************************************************************************/
/* PowerPC 601 specific instructions (POWER bridge) */

target_ulong helper_rac(CPUPPCState *env, target_ulong addr)
{
    mmu_ctx_t ctx;
    int nb_BATs;
    target_ulong ret = 0;

    /*
     * We don't have to generate many instances of this instruction,
     * as rac is supervisor only.
     *
     * XXX: FIX THIS: Pretend we have no BAT
     */
    nb_BATs = env->nb_BATs;
    env->nb_BATs = 0;
    if (get_physical_address(env, &ctx, addr, 0, ACCESS_INT) == 0) {
        ret = ctx.raddr;
    }
    env->nb_BATs = nb_BATs;
    return ret;
}

static inline target_ulong booke_tlb_to_page_size(int size)
{
    return 1024 << (2 * size);
}

static inline int booke_page_size_to_tlb(target_ulong page_size)
{
    int size;

    switch (page_size) {
    case 0x00000400UL:
        size = 0x0;
        break;
    case 0x00001000UL:
        size = 0x1;
        break;
    case 0x00004000UL:
        size = 0x2;
        break;
    case 0x00010000UL:
        size = 0x3;
        break;
    case 0x00040000UL:
        size = 0x4;
        break;
    case 0x00100000UL:
        size = 0x5;
        break;
    case 0x00400000UL:
        size = 0x6;
        break;
    case 0x01000000UL:
        size = 0x7;
        break;
    case 0x04000000UL:
        size = 0x8;
        break;
    case 0x10000000UL:
        size = 0x9;
        break;
    case 0x40000000UL:
        size = 0xA;
        break;
#if defined(TARGET_PPC64)
    case 0x000100000000ULL:
        size = 0xB;
        break;
    case 0x000400000000ULL:
        size = 0xC;
        break;
    case 0x001000000000ULL:
        size = 0xD;
        break;
    case 0x004000000000ULL:
        size = 0xE;
        break;
    case 0x010000000000ULL:
        size = 0xF;
        break;
#endif
    default:
        size = -1;
        break;
    }

    return size;
}

/* Helpers for 4xx TLB management */
#define PPC4XX_TLB_ENTRY_MASK       0x0000003f  /* Mask for 64 TLB entries */

#define PPC4XX_TLBHI_V              0x00000040
#define PPC4XX_TLBHI_E              0x00000020
#define PPC4XX_TLBHI_SIZE_MIN       0
#define PPC4XX_TLBHI_SIZE_MAX       7
#define PPC4XX_TLBHI_SIZE_DEFAULT   1
#define PPC4XX_TLBHI_SIZE_SHIFT     7
#define PPC4XX_TLBHI_SIZE_MASK      0x00000007

#define PPC4XX_TLBLO_EX             0x00000200
#define PPC4XX_TLBLO_WR             0x00000100
#define PPC4XX_TLBLO_ATTR_MASK      0x000000FF
#define PPC4XX_TLBLO_RPN_MASK       0xFFFFFC00

target_ulong helper_4xx_tlbre_hi(CPUPPCState *env, target_ulong entry)
{
    ppcemb_tlb_t *tlb;
    target_ulong ret;
    int size;

    entry &= PPC4XX_TLB_ENTRY_MASK;
    tlb = &env->tlb.tlbe[entry];
    ret = tlb->EPN;
    if (tlb->prot & PAGE_VALID) {
        ret |= PPC4XX_TLBHI_V;
    }
    size = booke_page_size_to_tlb(tlb->size);
    if (size < PPC4XX_TLBHI_SIZE_MIN || size > PPC4XX_TLBHI_SIZE_MAX) {
        size = PPC4XX_TLBHI_SIZE_DEFAULT;
    }
    ret |= size << PPC4XX_TLBHI_SIZE_SHIFT;
    env->spr[SPR_40x_PID] = tlb->PID;
    return ret;
}

target_ulong helper_4xx_tlbre_lo(CPUPPCState *env, target_ulong entry)
{
    ppcemb_tlb_t *tlb;
    target_ulong ret;

    entry &= PPC4XX_TLB_ENTRY_MASK;
    tlb = &env->tlb.tlbe[entry];
    ret = tlb->RPN;
    if (tlb->prot & PAGE_EXEC) {
        ret |= PPC4XX_TLBLO_EX;
    }
    if (tlb->prot & PAGE_WRITE) {
        ret |= PPC4XX_TLBLO_WR;
    }
    return ret;
}

void helper_4xx_tlbwe_hi(CPUPPCState *env, target_ulong entry,
                         target_ulong val)
{
    CPUState *cs = env_cpu(env);
    ppcemb_tlb_t *tlb;
    target_ulong page, end;

    LOG_SWTLB("%s entry %d val " TARGET_FMT_lx "\n", __func__, (int)entry,
              val);
    entry &= PPC4XX_TLB_ENTRY_MASK;
    tlb = &env->tlb.tlbe[entry];
    /* Invalidate previous TLB (if it's valid) */
    if (tlb->prot & PAGE_VALID) {
        end = tlb->EPN + tlb->size;
        LOG_SWTLB("%s: invalidate old TLB %d start " TARGET_FMT_lx " end "
                  TARGET_FMT_lx "\n", __func__, (int)entry, tlb->EPN, end);
        for (page = tlb->EPN; page < end; page += TARGET_PAGE_SIZE) {
            tlb_flush_page(cs, page);
        }
    }
    tlb->size = booke_tlb_to_page_size((val >> PPC4XX_TLBHI_SIZE_SHIFT)
                                       & PPC4XX_TLBHI_SIZE_MASK);
    /*
     * We cannot handle TLB size < TARGET_PAGE_SIZE.
     * If this ever occurs, we should implement TARGET_PAGE_BITS_VARY
     */
    if ((val & PPC4XX_TLBHI_V) && tlb->size < TARGET_PAGE_SIZE) {
        cpu_abort(cs, "TLB size " TARGET_FMT_lu " < %u "
                  "are not supported (%d)\n"
                  "Please implement TARGET_PAGE_BITS_VARY\n",
                  tlb->size, TARGET_PAGE_SIZE, (int)((val >> 7) & 0x7));
    }
    tlb->EPN = val & ~(tlb->size - 1);
    if (val & PPC4XX_TLBHI_V) {
        tlb->prot |= PAGE_VALID;
        if (val & PPC4XX_TLBHI_E) {
            /* XXX: TO BE FIXED */
            cpu_abort(cs,
                      "Little-endian TLB entries are not supported by now\n");
        }
    } else {
        tlb->prot &= ~PAGE_VALID;
    }
    tlb->PID = env->spr[SPR_40x_PID]; /* PID */
    LOG_SWTLB("%s: set up TLB %d RPN " TARGET_FMT_plx " EPN " TARGET_FMT_lx
              " size " TARGET_FMT_lx " prot %c%c%c%c PID %d\n", __func__,
              (int)entry, tlb->RPN, tlb->EPN, tlb->size,
              tlb->prot & PAGE_READ ? 'r' : '-',
              tlb->prot & PAGE_WRITE ? 'w' : '-',
              tlb->prot & PAGE_EXEC ? 'x' : '-',
              tlb->prot & PAGE_VALID ? 'v' : '-', (int)tlb->PID);
    /* Invalidate new TLB (if valid) */
    if (tlb->prot & PAGE_VALID) {
        end = tlb->EPN + tlb->size;
        LOG_SWTLB("%s: invalidate TLB %d start " TARGET_FMT_lx " end "
                  TARGET_FMT_lx "\n", __func__, (int)entry, tlb->EPN, end);
        for (page = tlb->EPN; page < end; page += TARGET_PAGE_SIZE) {
            tlb_flush_page(cs, page);
        }
    }
}

void helper_4xx_tlbwe_lo(CPUPPCState *env, target_ulong entry,
                         target_ulong val)
{
    ppcemb_tlb_t *tlb;

    LOG_SWTLB("%s entry %i val " TARGET_FMT_lx "\n", __func__, (int)entry,
              val);
    entry &= PPC4XX_TLB_ENTRY_MASK;
    tlb = &env->tlb.tlbe[entry];
    tlb->attr = val & PPC4XX_TLBLO_ATTR_MASK;
    tlb->RPN = val & PPC4XX_TLBLO_RPN_MASK;
    tlb->prot = PAGE_READ;
    if (val & PPC4XX_TLBLO_EX) {
        tlb->prot |= PAGE_EXEC;
    }
    if (val & PPC4XX_TLBLO_WR) {
        tlb->prot |= PAGE_WRITE;
    }
    LOG_SWTLB("%s: set up TLB %d RPN " TARGET_FMT_plx " EPN " TARGET_FMT_lx
              " size " TARGET_FMT_lx " prot %c%c%c%c PID %d\n", __func__,
              (int)entry, tlb->RPN, tlb->EPN, tlb->size,
              tlb->prot & PAGE_READ ? 'r' : '-',
              tlb->prot & PAGE_WRITE ? 'w' : '-',
              tlb->prot & PAGE_EXEC ? 'x' : '-',
              tlb->prot & PAGE_VALID ? 'v' : '-', (int)tlb->PID);
}

target_ulong helper_4xx_tlbsx(CPUPPCState *env, target_ulong address)
{
    return ppcemb_tlb_search(env, address, env->spr[SPR_40x_PID]);
}

/* PowerPC 440 TLB management */
void helper_440_tlbwe(CPUPPCState *env, uint32_t word, target_ulong entry,
                      target_ulong value)
{
    ppcemb_tlb_t *tlb;
    target_ulong EPN, RPN, size;
    int do_flush_tlbs;

    LOG_SWTLB("%s word %d entry %d value " TARGET_FMT_lx "\n",
              __func__, word, (int)entry, value);
    do_flush_tlbs = 0;
    entry &= 0x3F;
    tlb = &env->tlb.tlbe[entry];
    switch (word) {
    default:
        /* Just here to please gcc */
    case 0:
        EPN = value & 0xFFFFFC00;
        if ((tlb->prot & PAGE_VALID) && EPN != tlb->EPN) {
            do_flush_tlbs = 1;
        }
        tlb->EPN = EPN;
        size = booke_tlb_to_page_size((value >> 4) & 0xF);
        if ((tlb->prot & PAGE_VALID) && tlb->size < size) {
            do_flush_tlbs = 1;
        }
        tlb->size = size;
        tlb->attr &= ~0x1;
        tlb->attr |= (value >> 8) & 1;
        if (value & 0x200) {
            tlb->prot |= PAGE_VALID;
        } else {
            if (tlb->prot & PAGE_VALID) {
                tlb->prot &= ~PAGE_VALID;
                do_flush_tlbs = 1;
            }
        }
        tlb->PID = env->spr[SPR_440_MMUCR] & 0x000000FF;
        if (do_flush_tlbs) {
            tlb_flush(env_cpu(env));
        }
        break;
    case 1:
        RPN = value & 0xFFFFFC0F;
        if ((tlb->prot & PAGE_VALID) && tlb->RPN != RPN) {
            tlb_flush(env_cpu(env));
        }
        tlb->RPN = RPN;
        break;
    case 2:
        tlb->attr = (tlb->attr & 0x1) | (value & 0x0000FF00);
        tlb->prot = tlb->prot & PAGE_VALID;
        if (value & 0x1) {
            tlb->prot |= PAGE_READ << 4;
        }
        if (value & 0x2) {
            tlb->prot |= PAGE_WRITE << 4;
        }
        if (value & 0x4) {
            tlb->prot |= PAGE_EXEC << 4;
        }
        if (value & 0x8) {
            tlb->prot |= PAGE_READ;
        }
        if (value & 0x10) {
            tlb->prot |= PAGE_WRITE;
        }
        if (value & 0x20) {
            tlb->prot |= PAGE_EXEC;
        }
        break;
    }
}

target_ulong helper_440_tlbre(CPUPPCState *env, uint32_t word,
                              target_ulong entry)
{
    ppcemb_tlb_t *tlb;
    target_ulong ret;
    int size;

    entry &= 0x3F;
    tlb = &env->tlb.tlbe[entry];
    switch (word) {
    default:
        /* Just here to please gcc */
    case 0:
        ret = tlb->EPN;
        size = booke_page_size_to_tlb(tlb->size);
        if (size < 0 || size > 0xF) {
            size = 1;
        }
        ret |= size << 4;
        if (tlb->attr & 0x1) {
            ret |= 0x100;
        }
        if (tlb->prot & PAGE_VALID) {
            ret |= 0x200;
        }
        env->spr[SPR_440_MMUCR] &= ~0x000000FF;
        env->spr[SPR_440_MMUCR] |= tlb->PID;
        break;
    case 1:
        ret = tlb->RPN;
        break;
    case 2:
        ret = tlb->attr & ~0x1;
        if (tlb->prot & (PAGE_READ << 4)) {
            ret |= 0x1;
        }
        if (tlb->prot & (PAGE_WRITE << 4)) {
            ret |= 0x2;
        }
        if (tlb->prot & (PAGE_EXEC << 4)) {
            ret |= 0x4;
        }
        if (tlb->prot & PAGE_READ) {
            ret |= 0x8;
        }
        if (tlb->prot & PAGE_WRITE) {
            ret |= 0x10;
        }
        if (tlb->prot & PAGE_EXEC) {
            ret |= 0x20;
        }
        break;
    }
    return ret;
}

target_ulong helper_440_tlbsx(CPUPPCState *env, target_ulong address)
{
    return ppcemb_tlb_search(env, address, env->spr[SPR_440_MMUCR] & 0xFF);
}

/* PowerPC 476FP TLB management */

#define PPC476_TLB_PAGE_SIZE_MASK       0x3f0
#define PPC476_TLB_PAGE_SIZE_SHIFT      4
// translation space bit
#define PPC476_TLB_TS_BIT               0x400
#define PPC476_TLB_VALID_BIT            0x800
#define PPC476_TLB_EPN_SHIFT            12
#define PPC476_TLB_BOLTED_INDEX_MASK    0x7000000
#define PPC476_TLB_BOLTED_INDEX_SHIFT   24
#define PPC476_TLB_BOLTED_ENTRY         0x8000000
#define PPC476_TLB_MANUAL_WAY_MASK      0x60000000
#define PPC476_TLB_MANUAL_WAY_SHIFT     29
#define PPC476_TLB_MANUAL_WAY_SEL       0x80000000
#define PPC476_TLB_RPN_MASK             0xfffff000
#define PPC476_TLB_ERPN_MASK            0x3ff
#define PPC476_TLB_ERPN_SHIFT           32
#define PPC476_TLB_ACCESS_PARAMS        0x3ff80

#define PPC476_MMUCR_STID_MASK          0xffff
#define PPC476_MMUCR_LINDEX_MASK        0x1fe0000
#define PPC476_MMUCR_LINDEX_SHIFT       17
#define PPC476_MMUCR_LVALID_MASK        0x10000000
#define PPC476_MMUCR_LVALID_SHIFT       28
#define PPC476_MMUCR_LWAY_MASK          0x60000000
#define PPC476_MMUCR_LWAY_SHIFT         29

#define PPC476_MMUBE_VALID_0            0x4
#define PPC476_MMUBE_VALID_1            0x2
#define PPC476_MMUBE_VALID_2            0x1
#define PPC476_MMUBE_INDEX_MASK         0xff
#define PPC476_MMUBE_INDEX_SHIFT_0      24
#define PPC476_MMUBE_INDEX_SHIFT_1      16
#define PPC476_MMUBE_INDEX_SHIFT_2      8

/* Total number of bolted entries */
#define PPC476_BOLTED_ENTRY_COUNT       6

static int ppc476_tlb_search(CPUPPCState *env, target_ulong address,
                             uint32_t pid)
{
    ppcemb_tlb_t *tlb;
    hwaddr raddr;
    int i, ret;

    /* Default return value is no match */
    ret = -1;
    for (i = 0; i < env->nb_tlb; i++) {
        tlb = &env->tlb.tlbe[i];
        if (ppc476fp_tlb_check(env, tlb, &raddr, address, pid, i) == 0) {
            ret = i;
            break;
        }
    }

    return ret;
}

static void update_476_bolted_entry(CPUPPCState *env, int entry_num, uint32_t index)
{
    target_ulong *ptr;

    // first three entries are in MMUBE0, others three in MMUBE1
    ptr = entry_num < PPC476_BOLTED_ENTRY_COUNT / 2 ?
        &env->spr[SPR_476_MMUBE0] : &env->spr[SPR_476_MMUBE1];

    switch (entry_num % (PPC476_BOLTED_ENTRY_COUNT / 2)) {
    case 0:
        *ptr &= ~(PPC476_MMUBE_INDEX_MASK << PPC476_MMUBE_INDEX_SHIFT_0);
        *ptr |= (index & PPC476_MMUBE_INDEX_MASK) << PPC476_MMUBE_INDEX_SHIFT_0;
        *ptr |= PPC476_MMUBE_VALID_0;
        break;

    case 1:
        *ptr &= ~(PPC476_MMUBE_INDEX_MASK << PPC476_MMUBE_INDEX_SHIFT_1);
        *ptr |= (index & PPC476_MMUBE_INDEX_MASK) << PPC476_MMUBE_INDEX_SHIFT_1;
        *ptr |= PPC476_MMUBE_VALID_1;
        break;

    case 2:
        *ptr &= ~(PPC476_MMUBE_INDEX_MASK << PPC476_MMUBE_INDEX_SHIFT_2);
        *ptr |= (index & PPC476_MMUBE_INDEX_MASK) << PPC476_MMUBE_INDEX_SHIFT_2;
        *ptr |= PPC476_MMUBE_VALID_2;
        break;
    }
}

static void remove_476_bolted_entry(CPUPPCState *env, uint32_t index)
{
    target_ulong *regs[] = {&env->spr[SPR_476_MMUBE0], &env->spr[SPR_476_MMUBE1]};

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        target_ulong reg = *regs[i];

        for (int j = 0; j < PPC476_BOLTED_ENTRY_COUNT / 2; j++) {
            reg >>= PPC476_MMUBE_INDEX_SHIFT_2;

            if ((reg & PPC476_MMUBE_INDEX_MASK) == (index & PPC476_MMUBE_INDEX_MASK)) {
                *regs[i] &= ~(1 << j);
                return;
            }
        }
    }
}

/* Exclusive OR (XOR) based hash function is used when an entry is searched
 *
 * Entry bits values are calculated by using tid bits and address bits
 *
 * | entry bits    | 7| 6| 5| 4| 3| 2| 1| 0|
 * |---------------|--|--|--|--|--|--|--|--|
 * | tid bits      | 7| 6| 5| 4| 3| 2| 1| 0|
 * |---------------|--|--|--|--|--|--|--|--|
 * | address bits  |19|18|17|16|15|14|13|12|
 * |               |11|10| 9| 8|  |  |  |  |
 * |               | 7| 6| 5| 4| 3| 2| 1| 0|
 */
static inline uint8_t calc_476_tlb_entry_index(target_ulong addr, uint32_t tid, uint32_t size)
{
    switch (size) {
    case 0x00: return  ((tid & 0xff) ^ ((addr & 0xff000000) >> 24) ^ ((addr & 0xf00000) >> 16) ^ ((addr & 0xff000) >> 12));
    case 0x01: return  ((tid & 0xff) ^ ((addr & 0xff000000) >> 24) ^ ((addr & 0xC00000) >> 16) ^ ((addr & 0x3FC000) >> 14));
    case 0x03: return  ((tid & 0xff) ^ ((addr & 0xff000000) >> 24) ^ ((addr & 0xff0000) >> 16));
    case 0x07: return  ((tid & 0xff) ^ ((addr & 0xf0000000) >> 24) ^ ((addr & 0xff00000) >> 20));
    case 0x0f: return  ((tid & 0xff) ^ ((addr & 0xff000000) >> 24));
    case 0x1f: return  ((tid & 0xff) ^ ((addr & 0xf0000000) >> 24));
    case 0x3f: return  ((tid & 0xff) ^ ((addr & 0xC0000000) >> 24));
    default: return -1;
    }
    //return (tid & 0xff) ^ (addr & 0xff) ^ ((addr >> 4) & 0xf0) ^ ((addr >> 12) & 0xff);
}

/* Linearise ppc tlb representation in qemu internal tlb array */
static inline int calc_476_tlb_entry(uint32_t index, uint32_t way, int cnt)
{
    return index + way * cnt;
}

/* Unlinearise internal tlb index */
static inline target_ulong get_476_tlb_index_and_way(target_ulong entry, uint32_t cnt,
                                                     uint32_t *way)
{
    *way = entry / cnt;
    return entry % cnt;
}

static uint32_t calc_476_tlb_to_page_size(uint32_t size)
{
    switch (size) {
    case 0x00: return   4 * KiB;
    case 0x01: return  16 * KiB;
    case 0x03: return  64 * KiB;
    case 0x07: return   1 * MiB;
    case 0x0f: return  16 * MiB;
    case 0x1f: return 256 * MiB;
    case 0x3f: return   1 * GiB;
    default: return 0;
    }
}

static uint32_t calc_476_page_size_to_tlb(uint32_t size)
{
    switch (size) {
    case   4 * KiB: return 0x00;
    case  16 * KiB: return 0x01;
    case  64 * KiB: return 0x03;
    case   1 * MiB: return 0x07;
    case  16 * MiB: return 0x0f;
    case 256 * MiB: return 0x1f;
    case   1 * GiB: return 0x3f;
    default: return 0;
    }
}

void helper_476_tlbwe(CPUPPCState *env, uint32_t word, target_ulong entry,
                      target_ulong value)
{
    uint32_t way, index, tid;
    uint32_t addr, size, valid;
    ppcemb_tlb_t *tlb;
    // FIXME: нормально ли так сохранять инфу об этом?
    static int increment_hardware_way;
    static int bolted_entry_num;

    LOG_SWTLB("%s word %d entry %x value " TARGET_FMT_lx "\n",
        __func__, word, (unsigned)entry, value);

    switch (word) {
    default:
    case 0:
        size = calc_476_tlb_to_page_size(
                (value & PPC476_TLB_PAGE_SIZE_MASK) >> PPC476_TLB_PAGE_SIZE_SHIFT);
        addr = value & ~(size - 1);
        valid = value & PPC476_TLB_VALID_BIT ? 1 : 0;

        tid = env->spr[SPR_440_MMUCR] & PPC476_MMUCR_STID_MASK;

        index = calc_476_tlb_entry_index(addr, tid, calc_476_page_size_to_tlb(size));
        //index = calc_476_tlb_entry_index(addr >> PPC476_TLB_EPN_SHIFT, tid);

        if (entry & PPC476_TLB_BOLTED_ENTRY) {
            way = 0;
            bolted_entry_num =
                (entry & PPC476_TLB_BOLTED_INDEX_MASK) >> PPC476_TLB_BOLTED_INDEX_SHIFT;
        } else if (entry & PPC476_TLB_MANUAL_WAY_SEL) {
            way = (entry & PPC476_TLB_MANUAL_WAY_MASK) >> PPC476_TLB_MANUAL_WAY_SHIFT;
        } else {
            way = env->tlb_way_selection[index];
        }

        increment_hardware_way =
            !(entry & PPC476_TLB_BOLTED_ENTRY || entry & PPC476_TLB_MANUAL_WAY_SEL || !valid);

        // get tlb entry pointer
        tlb = &env->tlb.tlbe[calc_476_tlb_entry(index, way, env->tlb_per_way)];

        // select next way if Hardware Assisted Way Selection got us a BOLTED entry
        if (!(entry & PPC476_TLB_BOLTED_ENTRY) && !(entry & PPC476_TLB_MANUAL_WAY_SEL) &&
            (tlb->attr & PPC476_TLB_BOLTED_ENTRY)) {
            way = ++env->tlb_way_selection[index];
            tlb = &env->tlb.tlbe[calc_476_tlb_entry(index, way, env->tlb_per_way)];
        }

        // remove old index, valid bit and way
        env->spr[SPR_440_MMUCR] &=
            ~(PPC476_MMUCR_LVALID_MASK | PPC476_MMUCR_LWAY_MASK | PPC476_MMUCR_LINDEX_MASK);

        env->spr[SPR_440_MMUCR] |= index << PPC476_MMUCR_LINDEX_SHIFT;
        env->spr[SPR_440_MMUCR] |= valid << PPC476_MMUCR_LVALID_SHIFT;
        env->spr[SPR_440_MMUCR] |= way << PPC476_MMUCR_LWAY_SHIFT;

        tlb->prot &= ~PAGE_VALID;

        if (valid) {
            tlb->EPN = addr;
            tlb->size = size;
            tlb->PID = tid;
            // make it BOLTED if it should be
            tlb->attr = entry & PPC476_TLB_BOLTED_ENTRY;
            // update TS bit
            tlb->attr &= ~0x1;
            tlb->attr |= value & PPC476_TLB_TS_BIT ? 0x1 : 0;
        } else {
            // we just invalidated an entry so this way is free for next entry
            env->tlb_way_selection[index] = way;

            // we can invalidate entry with only one tlbwe command
            tlb_flush(env_cpu(env));

            // update MMUBE0 or MMUBE1 if this entry is bolted
            if (tlb->attr & PPC476_TLB_BOLTED_ENTRY) {
                remove_476_bolted_entry(env, index);
            }
        }
        break;

    case 1:
        index =
            (env->spr[SPR_440_MMUCR] & PPC476_MMUCR_LINDEX_MASK) >> PPC476_MMUCR_LINDEX_SHIFT;
        way =
            (env->spr[SPR_440_MMUCR] & PPC476_MMUCR_LWAY_MASK) >> PPC476_MMUCR_LWAY_SHIFT;
        tlb = &env->tlb.tlbe[calc_476_tlb_entry(index, way, env->tlb_per_way)];

        tlb->RPN = value & PPC476_TLB_ERPN_MASK;
        tlb->RPN <<= PPC476_TLB_ERPN_SHIFT;
        tlb->RPN |= value & PPC476_TLB_RPN_MASK;
        break;

    case 2:
        index =
            (env->spr[SPR_440_MMUCR] & PPC476_MMUCR_LINDEX_MASK) >> PPC476_MMUCR_LINDEX_SHIFT;
        way =
            (env->spr[SPR_440_MMUCR] & PPC476_MMUCR_LWAY_MASK) >> PPC476_MMUCR_LWAY_SHIFT;
        tlb = &env->tlb.tlbe[calc_476_tlb_entry(index, way, env->tlb_per_way)];

        // make it bolted if it was
        tlb->attr = (value & PPC476_TLB_ACCESS_PARAMS) |
            (tlb->attr & PPC476_TLB_BOLTED_ENTRY) | (tlb->attr & 0x1);

        if (increment_hardware_way) {
            env->tlb_way_selection[index]++;
            env->tlb_way_selection[index] %= env->nb_ways;
        }

        tlb->prot = 0;

        if (value & 0x1) {
            tlb->prot |= PAGE_READ << 4;
        }
        if (value & 0x2) {
            tlb->prot |= PAGE_WRITE << 4;
        }
        if (value & 0x4) {
            tlb->prot |= PAGE_EXEC << 4;
        }
        if (value & 0x8) {
            tlb->prot |= PAGE_READ;
        }
        if (value & 0x10) {
            tlb->prot |= PAGE_WRITE;
        }
        if (value & 0x20) {
            tlb->prot |= PAGE_EXEC;
        }

        if (env->spr[SPR_440_MMUCR] & PPC476_MMUCR_LVALID_MASK) {
            tlb->prot |= PAGE_VALID;

            // update MMUBE0 or MMUBE1 if this entry is bolted
            if (tlb->attr & PPC476_TLB_BOLTED_ENTRY) {
                update_476_bolted_entry(env, bolted_entry_num, index);
            }
        }
        tlb_flush(env_cpu(env));
        break;
    }
}

target_ulong helper_476_tlbre(CPUPPCState *env, uint32_t word,
                              target_ulong entry)
{
    ppcemb_tlb_t *tlb;
    uint32_t way, index;
    target_ulong ret;

    way = (entry & PPC476_TLB_MANUAL_WAY_MASK) >> PPC476_TLB_MANUAL_WAY_SHIFT;
    index = (entry & 0xff0000) >> 16;

    tlb = &env->tlb.tlbe[calc_476_tlb_entry(index, way, env->tlb_per_way)];
    switch (word) {
    default:
        /* Just here to please gcc */
    case 0:
        ret = tlb->EPN;
        if (tlb->prot & PAGE_VALID) {
            ret |= PPC476_TLB_VALID_BIT;
        }
        if (tlb->attr & 0x1) {
            ret |= PPC476_TLB_TS_BIT;
        }
        ret |= calc_476_page_size_to_tlb(tlb->size) << PPC476_TLB_PAGE_SIZE_SHIFT;
        if (tlb->attr & PPC476_TLB_BOLTED_ENTRY) {
            ret |= 1 << 3;
        }

        env->spr[SPR_440_MMUCR] &= ~PPC476_MMUCR_STID_MASK;
        env->spr[SPR_440_MMUCR] |= tlb->PID;
        break;
    case 1:
        ret = tlb->RPN & PPC476_TLB_RPN_MASK;
        ret |= (tlb->RPN >> PPC476_TLB_ERPN_SHIFT) & PPC476_TLB_ERPN_MASK;
        break;
    case 2:
        ret = tlb->attr & PPC476_TLB_ACCESS_PARAMS;
        if (tlb->prot & (PAGE_READ << 4)) {
            ret |= 0x1;
        }
        if (tlb->prot & (PAGE_WRITE << 4)) {
            ret |= 0x2;
        }
        if (tlb->prot & (PAGE_EXEC << 4)) {
            ret |= 0x4;
        }
        if (tlb->prot & PAGE_READ) {
            ret |= 0x8;
        }
        if (tlb->prot & PAGE_WRITE) {
            ret |= 0x10;
        }
        if (tlb->prot & PAGE_EXEC) {
            ret |= 0x20;
        }
        break;
    }
    return ret;
}

target_ulong helper_476_tlbsx(CPUPPCState *env, target_ulong address)
{
    target_ulong entry = ppc476_tlb_search(env, address,
        env->spr[SPR_440_MMUCR] & PPC476_MMUCR_STID_MASK);

    uint32_t way;
    target_ulong result = get_476_tlb_index_and_way(entry, env->tlb_per_way, &way);

    result <<= 16;
    result |= way << 29;

    return result;
}

void helper_476_shadow_tlb_flush(CPUPPCState *env)
{
    env->curr_d_shadow_tlb = 0;
    env->curr_i_shadow_tlb = 0;
    env->last_d_shadow_tlb = 0;
    env->last_i_shadow_tlb = 0;
}

/* PowerPC BookE 2.06 TLB management */

static ppcmas_tlb_t *booke206_cur_tlb(CPUPPCState *env)
{
    uint32_t tlbncfg = 0;
    int esel = (env->spr[SPR_BOOKE_MAS0] & MAS0_ESEL_MASK) >> MAS0_ESEL_SHIFT;
    int ea = (env->spr[SPR_BOOKE_MAS2] & MAS2_EPN_MASK);
    int tlb;

    tlb = (env->spr[SPR_BOOKE_MAS0] & MAS0_TLBSEL_MASK) >> MAS0_TLBSEL_SHIFT;
    tlbncfg = env->spr[SPR_BOOKE_TLB0CFG + tlb];

    if ((tlbncfg & TLBnCFG_HES) && (env->spr[SPR_BOOKE_MAS0] & MAS0_HES)) {
        cpu_abort(env_cpu(env), "we don't support HES yet\n");
    }

    return booke206_get_tlbm(env, tlb, ea, esel);
}

void helper_booke_setpid(CPUPPCState *env, uint32_t pidn, target_ulong pid)
{
    env->spr[pidn] = pid;
    /* changing PIDs mean we're in a different address space now */
    tlb_flush(env_cpu(env));
}

void helper_booke_set_eplc(CPUPPCState *env, target_ulong val)
{
    env->spr[SPR_BOOKE_EPLC] = val & EPID_MASK;
    tlb_flush_by_mmuidx(env_cpu(env), 1 << PPC_TLB_EPID_LOAD);
}
void helper_booke_set_epsc(CPUPPCState *env, target_ulong val)
{
    env->spr[SPR_BOOKE_EPSC] = val & EPID_MASK;
    tlb_flush_by_mmuidx(env_cpu(env), 1 << PPC_TLB_EPID_STORE);
}

static inline void flush_page(CPUPPCState *env, ppcmas_tlb_t *tlb)
{
    if (booke206_tlb_to_page_size(env, tlb) == TARGET_PAGE_SIZE) {
        tlb_flush_page(env_cpu(env), tlb->mas2 & MAS2_EPN_MASK);
    } else {
        tlb_flush(env_cpu(env));
    }
}

void helper_booke206_tlbwe(CPUPPCState *env)
{
    uint32_t tlbncfg, tlbn;
    ppcmas_tlb_t *tlb;
    uint32_t size_tlb, size_ps;
    target_ulong mask;


    switch (env->spr[SPR_BOOKE_MAS0] & MAS0_WQ_MASK) {
    case MAS0_WQ_ALWAYS:
        /* good to go, write that entry */
        break;
    case MAS0_WQ_COND:
        /* XXX check if reserved */
        if (0) {
            return;
        }
        break;
    case MAS0_WQ_CLR_RSRV:
        /* XXX clear entry */
        return;
    default:
        /* no idea what to do */
        return;
    }

    if (((env->spr[SPR_BOOKE_MAS0] & MAS0_ATSEL) == MAS0_ATSEL_LRAT) &&
        !msr_gs) {
        /* XXX we don't support direct LRAT setting yet */
        fprintf(stderr, "cpu: don't support LRAT setting yet\n");
        return;
    }

    tlbn = (env->spr[SPR_BOOKE_MAS0] & MAS0_TLBSEL_MASK) >> MAS0_TLBSEL_SHIFT;
    tlbncfg = env->spr[SPR_BOOKE_TLB0CFG + tlbn];

    tlb = booke206_cur_tlb(env);

    if (!tlb) {
        raise_exception_err_ra(env, POWERPC_EXCP_PROGRAM,
                               POWERPC_EXCP_INVAL |
                               POWERPC_EXCP_INVAL_INVAL, GETPC());
    }

    /* check that we support the targeted size */
    size_tlb = (env->spr[SPR_BOOKE_MAS1] & MAS1_TSIZE_MASK) >> MAS1_TSIZE_SHIFT;
    size_ps = booke206_tlbnps(env, tlbn);
    if ((env->spr[SPR_BOOKE_MAS1] & MAS1_VALID) && (tlbncfg & TLBnCFG_AVAIL) &&
        !(size_ps & (1 << size_tlb))) {
        raise_exception_err_ra(env, POWERPC_EXCP_PROGRAM,
                               POWERPC_EXCP_INVAL |
                               POWERPC_EXCP_INVAL_INVAL, GETPC());
    }

    if (msr_gs) {
        cpu_abort(env_cpu(env), "missing HV implementation\n");
    }

    if (tlb->mas1 & MAS1_VALID) {
        /*
         * Invalidate the page in QEMU TLB if it was a valid entry.
         *
         * In "PowerPC e500 Core Family Reference Manual, Rev. 1",
         * Section "12.4.2 TLB Write Entry (tlbwe) Instruction":
         * (https://www.nxp.com/docs/en/reference-manual/E500CORERM.pdf)
         *
         * "Note that when an L2 TLB entry is written, it may be displacing an
         * already valid entry in the same L2 TLB location (a victim). If a
         * valid L1 TLB entry corresponds to the L2 MMU victim entry, that L1
         * TLB entry is automatically invalidated."
         */
        flush_page(env, tlb);
    }

    tlb->mas7_3 = ((uint64_t)env->spr[SPR_BOOKE_MAS7] << 32) |
        env->spr[SPR_BOOKE_MAS3];
    tlb->mas1 = env->spr[SPR_BOOKE_MAS1];

    if ((env->spr[SPR_MMUCFG] & MMUCFG_MAVN) == MMUCFG_MAVN_V2) {
        /* For TLB which has a fixed size TSIZE is ignored with MAV2 */
        booke206_fixed_size_tlbn(env, tlbn, tlb);
    } else {
        if (!(tlbncfg & TLBnCFG_AVAIL)) {
            /* force !AVAIL TLB entries to correct page size */
            tlb->mas1 &= ~MAS1_TSIZE_MASK;
            /* XXX can be configured in MMUCSR0 */
            tlb->mas1 |= (tlbncfg & TLBnCFG_MINSIZE) >> 12;
        }
    }

    /* Make a mask from TLB size to discard invalid bits in EPN field */
    mask = ~(booke206_tlb_to_page_size(env, tlb) - 1);
    /* Add a mask for page attributes */
    mask |= MAS2_ACM | MAS2_VLE | MAS2_W | MAS2_I | MAS2_M | MAS2_G | MAS2_E;

    if (!msr_cm) {
        /*
         * Executing a tlbwe instruction in 32-bit mode will set bits
         * 0:31 of the TLB EPN field to zero.
         */
        mask &= 0xffffffff;
    }

    tlb->mas2 = env->spr[SPR_BOOKE_MAS2] & mask;

    if (!(tlbncfg & TLBnCFG_IPROT)) {
        /* no IPROT supported by TLB */
        tlb->mas1 &= ~MAS1_IPROT;
    }

    flush_page(env, tlb);
}

static inline void booke206_tlb_to_mas(CPUPPCState *env, ppcmas_tlb_t *tlb)
{
    int tlbn = booke206_tlbm_to_tlbn(env, tlb);
    int way = booke206_tlbm_to_way(env, tlb);

    env->spr[SPR_BOOKE_MAS0] = tlbn << MAS0_TLBSEL_SHIFT;
    env->spr[SPR_BOOKE_MAS0] |= way << MAS0_ESEL_SHIFT;
    env->spr[SPR_BOOKE_MAS0] |= env->last_way << MAS0_NV_SHIFT;

    env->spr[SPR_BOOKE_MAS1] = tlb->mas1;
    env->spr[SPR_BOOKE_MAS2] = tlb->mas2;
    env->spr[SPR_BOOKE_MAS3] = tlb->mas7_3;
    env->spr[SPR_BOOKE_MAS7] = tlb->mas7_3 >> 32;
}

void helper_booke206_tlbre(CPUPPCState *env)
{
    ppcmas_tlb_t *tlb = NULL;

    tlb = booke206_cur_tlb(env);
    if (!tlb) {
        env->spr[SPR_BOOKE_MAS1] = 0;
    } else {
        booke206_tlb_to_mas(env, tlb);
    }
}

void helper_booke206_tlbsx(CPUPPCState *env, target_ulong address)
{
    ppcmas_tlb_t *tlb = NULL;
    int i, j;
    hwaddr raddr;
    uint32_t spid, sas;

    spid = (env->spr[SPR_BOOKE_MAS6] & MAS6_SPID_MASK) >> MAS6_SPID_SHIFT;
    sas = env->spr[SPR_BOOKE_MAS6] & MAS6_SAS;

    for (i = 0; i < BOOKE206_MAX_TLBN; i++) {
        int ways = booke206_tlb_ways(env, i);

        for (j = 0; j < ways; j++) {
            tlb = booke206_get_tlbm(env, i, address, j);

            if (!tlb) {
                continue;
            }

            if (ppcmas_tlb_check(env, tlb, &raddr, address, spid)) {
                continue;
            }

            if (sas != ((tlb->mas1 & MAS1_TS) >> MAS1_TS_SHIFT)) {
                continue;
            }

            booke206_tlb_to_mas(env, tlb);
            return;
        }
    }

    /* no entry found, fill with defaults */
    env->spr[SPR_BOOKE_MAS0] = env->spr[SPR_BOOKE_MAS4] & MAS4_TLBSELD_MASK;
    env->spr[SPR_BOOKE_MAS1] = env->spr[SPR_BOOKE_MAS4] & MAS4_TSIZED_MASK;
    env->spr[SPR_BOOKE_MAS2] = env->spr[SPR_BOOKE_MAS4] & MAS4_WIMGED_MASK;
    env->spr[SPR_BOOKE_MAS3] = 0;
    env->spr[SPR_BOOKE_MAS7] = 0;

    if (env->spr[SPR_BOOKE_MAS6] & MAS6_SAS) {
        env->spr[SPR_BOOKE_MAS1] |= MAS1_TS;
    }

    env->spr[SPR_BOOKE_MAS1] |= (env->spr[SPR_BOOKE_MAS6] >> 16)
        << MAS1_TID_SHIFT;

    /* next victim logic */
    env->spr[SPR_BOOKE_MAS0] |= env->last_way << MAS0_ESEL_SHIFT;
    env->last_way++;
    env->last_way &= booke206_tlb_ways(env, 0) - 1;
    env->spr[SPR_BOOKE_MAS0] |= env->last_way << MAS0_NV_SHIFT;
}

static inline void booke206_invalidate_ea_tlb(CPUPPCState *env, int tlbn,
                                              vaddr ea)
{
    int i;
    int ways = booke206_tlb_ways(env, tlbn);
    target_ulong mask;

    for (i = 0; i < ways; i++) {
        ppcmas_tlb_t *tlb = booke206_get_tlbm(env, tlbn, ea, i);
        if (!tlb) {
            continue;
        }
        mask = ~(booke206_tlb_to_page_size(env, tlb) - 1);
        if (((tlb->mas2 & MAS2_EPN_MASK) == (ea & mask)) &&
            !(tlb->mas1 & MAS1_IPROT)) {
            tlb->mas1 &= ~MAS1_VALID;
        }
    }
}

void helper_booke206_tlbivax(CPUPPCState *env, target_ulong address)
{
    CPUState *cs;

    if (address & 0x4) {
        /* flush all entries */
        if (address & 0x8) {
            /* flush all of TLB1 */
            booke206_flush_tlb(env, BOOKE206_FLUSH_TLB1, 1);
        } else {
            /* flush all of TLB0 */
            booke206_flush_tlb(env, BOOKE206_FLUSH_TLB0, 0);
        }
        return;
    }

    if (address & 0x8) {
        /* flush TLB1 entries */
        booke206_invalidate_ea_tlb(env, 1, address);
        CPU_FOREACH(cs) {
            tlb_flush(cs);
        }
    } else {
        /* flush TLB0 entries */
        booke206_invalidate_ea_tlb(env, 0, address);
        CPU_FOREACH(cs) {
            tlb_flush_page(cs, address & MAS2_EPN_MASK);
        }
    }
}

void helper_booke206_tlbilx0(CPUPPCState *env, target_ulong address)
{
    /* XXX missing LPID handling */
    booke206_flush_tlb(env, -1, 1);
}

void helper_booke206_tlbilx1(CPUPPCState *env, target_ulong address)
{
    int i, j;
    int tid = (env->spr[SPR_BOOKE_MAS6] & MAS6_SPID);
    ppcmas_tlb_t *tlb = env->tlb.tlbm;
    int tlb_size;

    /* XXX missing LPID handling */
    for (i = 0; i < BOOKE206_MAX_TLBN; i++) {
        tlb_size = booke206_tlb_size(env, i);
        for (j = 0; j < tlb_size; j++) {
            if (!(tlb[j].mas1 & MAS1_IPROT) &&
                ((tlb[j].mas1 & MAS1_TID_MASK) == tid)) {
                tlb[j].mas1 &= ~MAS1_VALID;
            }
        }
        tlb += booke206_tlb_size(env, i);
    }
    tlb_flush(env_cpu(env));
}

void helper_booke206_tlbilx3(CPUPPCState *env, target_ulong address)
{
    int i, j;
    ppcmas_tlb_t *tlb;
    int tid = (env->spr[SPR_BOOKE_MAS6] & MAS6_SPID);
    int pid = tid >> MAS6_SPID_SHIFT;
    int sgs = env->spr[SPR_BOOKE_MAS5] & MAS5_SGS;
    int ind = (env->spr[SPR_BOOKE_MAS6] & MAS6_SIND) ? MAS1_IND : 0;
    /* XXX check for unsupported isize and raise an invalid opcode then */
    int size = env->spr[SPR_BOOKE_MAS6] & MAS6_ISIZE_MASK;
    /* XXX implement MAV2 handling */
    bool mav2 = false;

    /* XXX missing LPID handling */
    /* flush by pid and ea */
    for (i = 0; i < BOOKE206_MAX_TLBN; i++) {
        int ways = booke206_tlb_ways(env, i);

        for (j = 0; j < ways; j++) {
            tlb = booke206_get_tlbm(env, i, address, j);
            if (!tlb) {
                continue;
            }
            if ((ppcmas_tlb_check(env, tlb, NULL, address, pid) != 0) ||
                (tlb->mas1 & MAS1_IPROT) ||
                ((tlb->mas1 & MAS1_IND) != ind) ||
                ((tlb->mas8 & MAS8_TGS) != sgs)) {
                continue;
            }
            if (mav2 && ((tlb->mas1 & MAS1_TSIZE_MASK) != size)) {
                /* XXX only check when MMUCFG[TWC] || TLBnCFG[HES] */
                continue;
            }
            /* XXX e500mc doesn't match SAS, but other cores might */
            tlb->mas1 &= ~MAS1_VALID;
        }
    }
    tlb_flush(env_cpu(env));
}

void helper_booke206_tlbflush(CPUPPCState *env, target_ulong type)
{
    int flags = 0;

    if (type & 2) {
        flags |= BOOKE206_FLUSH_TLB1;
    }

    if (type & 4) {
        flags |= BOOKE206_FLUSH_TLB0;
    }

    booke206_flush_tlb(env, flags, 1);
}


void helper_check_tlb_flush_local(CPUPPCState *env)
{
    check_tlb_flush(env, false);
}

void helper_check_tlb_flush_global(CPUPPCState *env)
{
    check_tlb_flush(env, true);
}


bool ppc_cpu_tlb_fill(CPUState *cs, vaddr eaddr, int size,
                      MMUAccessType access_type, int mmu_idx,
                      bool probe, uintptr_t retaddr)
{
    PowerPCCPU *cpu = POWERPC_CPU(cs);
    hwaddr raddr;
    int page_size, prot;

    if (ppc_xlate(cpu, eaddr, access_type, &raddr,
                  &page_size, &prot, mmu_idx, !probe)) {
        // FIXME: depending on CPU endian mode shouldn't we reverse this bit ???
        if (prot & PAGE_LE) {
            tlb_set_page_with_attrs(cs, eaddr & TARGET_PAGE_MASK, raddr & TARGET_PAGE_MASK,
                                    (MemTxAttrs) { .byte_swap = 1 },
                                    prot, mmu_idx, 1UL << page_size);
        } else {
            tlb_set_page(cs, eaddr & TARGET_PAGE_MASK, raddr & TARGET_PAGE_MASK,
                         prot, mmu_idx, 1UL << page_size);
        }
        return true;
    }
    if (probe) {
        return false;
    }
    raise_exception_err_ra(&cpu->env, cs->exception_index,
                           cpu->env.error_code, retaddr);
}
