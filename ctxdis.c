/*************************************************************************
 *                                                                       *
 *             DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE               *
 *                     Version 2, December 2004                          *
 *                                                                       *
 *  Copyright (C) 2004 Sam Hocevar                                       *
 *   14 rue de Plaisance, 75014 Paris, France                            *
 *  Everyone is permitted to copy and distribute verbatim or modified    *
 *  copies of this license document, and changing it is allowed as long  *
 *  as the name is changed.                                              *
 *                                                                       *
 *             DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE               *
 *    TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION    *
 *                                                                       *
 *   0. You just DO WHAT THE FUCK YOU WANT TO.                           *
 *                                                                       *
 *************************************************************************/

/*
 * Copyright (C) 2009 Marcin Kościelnicki <koriakin@0x04.net>
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

/*
 * PGRAPH registers of interest
 *
 * 0x400300: some sort of status register... shows 0xe when execution stopped,
 *           0xX3 when running or paused.
 * 0x400304: control register. For writes:
 *           bit 0: start execution
 *           bit 1: resume execution
 *           bit 4: pause execution
 *           For reads:
 *           bit 0: prog running or paused
 * 0x400308: Next opcode to execute, or the exit opcode if program exitted.
 * 0x40030c: RO alias of PC for some reason
 * 0x400310: PC. Readable and writable. In units of single insns.
 * 0x40031c: the scratch register, written by opcode 2, holding params to
 *           various stuff.
 * 0x400324: code upload index, WO. selects address to write in ctxprog code,
 *           counted in whole insns.
 * 0x400328: code upload, WO. writes given insn to code segment and
 *           autoincrements upload address.
 * 0x400334: $r register, offset in RAMIN context to read/write with opcode 1.
 *           in units of 32-bit words.
 * 0x400338: Some register, set with opcode 3. Used on NVAx with values 0-9 to
 *           select TP number to read/write with G[0x8000]. Maybe generic
 *           PGRAPH offset? Also set to 0x8ffff in one place for some reason.
 * 0x40033c: Some other register. Set with opcode 0x600007. Related to and
 *           modified by opcode 8. Setting with opcode 0x600007 always ands
 *           contents with 0xffff8 for some reason.
 * 0x400824-0x400830: Flags, see below.
 *
 * How execution works:
 *
 * Execution can be in one of 3 states: stopped, running, and paused.
 * Initially, it's stopped. The only way to start execution is to poke 0x400304
 * with bit 0 set. When you do this, the uc starts executing opcodes from
 * current PC. The only way to stop execution is to hit opcode 0x60000c.
 * Note that 0x60000c resets PC to 0, so ctxprog will start over from there
 * next time, unless you poke PC before that. Running <-> paused transition
 * happens by poking 0x400304 with the appropriate bit set to 1. Poking with
 * *both* bits set to 1 causes it to reload the opcode register from current
 * PC, but nothing else happens. This can be used to read the code segment.
 * Poking with bits 4 and 0 set together causes it to start with paused state.
 *
 * Oh, also, PC has 9 valid bits. So you can have ctxprogs of max. 512 insns.
 */

/*
 * All of the above has been checked on my NV86 laptop card and will be
 * checked soon on my NVA5. Should probably apply to all NV50 family cards.
 * NV40 is rather different, but should be somewhat similar, needs to be
 * checked some day.
 */

#define NV4x 1
#define NV5x 2 /* and 8x and 9x and Ax */
#define NVxx 3

/*
 * Color scheme
 */

char cnorm[] = "\x1b[0m";	// lighgray: instruction code and misc stuff
char cgray[] = "\x1b[37m";	// darkgray: instruction address
char cgr[] = "\x1b[32m";	// green: instruction name and mods
char cbl[] = "\x1b[34m";	// blue: $r registers
char ccy[] = "\x1b[36m";	// cyan: memory accesses
char cyel[] = "\x1b[33m";	// yellow: numbers
char cred[] = "\x1b[31m";	// red: unknown stuff
char cbr[] = "\x1b[37m";	// white: code labels
char cmag[] = "\x1b[35m";	// pink: funny registers


/*
 * Table format
 *
 * Decoding various fields is done by multi-level tables of decode operations.
 * Fields of a single table entry:
 *  - bitmask of program types for which this entry applies
 *  - value that needs to match
 *  - mask of bits in opcode that need to match the val
 *  - a sequence of 0 to 8 operations to perform if this entry is matched.
 *
 * Each table is scanned linearly until a matching entry is found, then all
 * ops in this entry are executed. Length of a table is not checked, so they
 * need either a terminator showing '???' for unknown stuff, or match all
 * possible values.
 *
 * A single op is supposed to decode some field of the instruction and print
 * it. In the table, an op is just a function pointer plus a void* that gets
 * passed to it as an argument. atomtab is an op that descends recursively
 * into another table to decude a subfield. T(too) is an op shorthand for use
 * in tables.
 *
 * Each op takes five arguments: output file to print to, pointer to the
 * opcode being decoded, pointer to a mask value of already-used fields,
 * a free-form void * specified in table entry that called this op, and
 * allowed program type bitmask.
 *
 * The mask argument is supposed to detect unknown fields in opcodes: if some
 * bit in decoded opcode is set, but no op executed along the way claims to
 * use it, it's probably some unknown field that could totally change the
 * meaning of instruction and will be printed in bold fucking red to alert
 * the user. Each op ors in the bitmask that it uses, and after all ops are
 * executed, main code prints unclaimed bits. This works for fields that can
 * be used multiple times, like the COND field: it'll be read fine by all ops
 * that happen to use it, but will be marked red if no op used it, like
 * an unconditional non-addc non-mov-from-$c insn.
 *
 * This doesn't work for $a, as there is a special case here: only the first
 * field using it actually gets its value, next ones just use 0. This is
 * hacked around by zeroing out the field directly in passed opcode parameter
 * in the op reading it. This gives proper behavior on stuff like add $r0,
 * s[$a1+0x4], c0[0x10].
 *
 * Macros provided for quickly getting the bitfields: BF(...) gets value of
 * given bitfield and marks it as used in the mask, RCL(...) wipes it
 * from the opcode field directly, allowing for the $a special case. Args are
 * given as start bit, size in bits.
 *
 * Also, three simple ops are provided: N("string") prints a literal to output
 * and is supposed to be used for instruction names and various modifiers.
 * OOPS is for unknown encodings, NL is for separating instructions in case
 * a single opcode represents two [only possible with join and exit].
 */

typedef unsigned long long ull;

#define BF(s, l) (*m |= ((1ull<<l)-1<<s), *a>>s&(1ull<<l)-1)
#define RCL(s, l) (*a &= ~((1ull<<l)-1<<s))

#define APROTO (FILE *out, ull *a, ull *m, const void *v, int ptype)

typedef void (*afun) APROTO;

struct atom {
	afun fun;
	const void *arg;
};

struct insn {
	int ptype;
	ull val;
	ull mask;
	struct atom atoms[10];
};

/*
 * Makes a simple table for checking a single flag.
 *
 * Arguments: table name, flag position, ops for 0, ops for 1.
 */

#define F(n, f, a, b) struct insn tab ## n[] = {\
	{ AP, 0,		1ull<<(f), a },\
	{ AP, 1ull<<(f),	1ull<<(f), b },\
};
#define F1(n, f, b) struct insn tab ## n[] = {\
	{ AP, 0,		1ull<<(f) },\
	{ AP, 1ull<<(f),	1ull<<(f), b },\
};


#define T(x) atomtab, tab ## x
void atomtab APROTO {
	const struct insn *tab = v;
	int i;
	while ((a[0]&tab->mask) != tab->val || !(tab->ptype&ptype))
		tab++;
	m[0] |= tab->mask;
	for (i = 0; i < 10; i++)
		if (tab->atoms[i].fun)
			tab->atoms[i].fun (out, a, m, tab->atoms[i].arg, ptype);
}

#define N(x) atomname, x
void atomname APROTO {
	fprintf (out, " %s%s", cgr, (char *)v);
}

#define OOPS atomoops, 0
void atomoops APROTO {
	fprintf (out, " %s???", cred);
}

/*
 * Code target field
 *
 * This field represents a code address and is used for branching and the
 * likes. Target is counted in 32-bit words from the start of microcode.
 */

#define CTARG atomctarg, 0
void atomctarg APROTO {
	fprintf (out, " %s%#llx", cbr, BF(8, 9)<<2);
}

/*
 * Misc number fields
 *
 * Used for plain numerical arguments.
 */

// BF, shift

int unitoff[] = { 0, 5, 0 };
int flagoff[] = { 0, 7, 0 };
int gsize4off[] = { 14, 6, 0 };
int gsize5off[] = { 16, 4, 0 };
int immoff[] = { 0, 20, 0 };
int goffoff[] = { 0, 20, 0 };
#define UNIT atomnum, unitoff
#define FLAG atomnum, flagoff
#define GSIZE4 atomnum, gsize4off
#define GSIZE5 atomnum, gsize5off
#define IMM atomnum, immoff
#define GOFF atomnum, goffoff
void atomnum APROTO {
	const int *n = v;
	uint32_t num = BF(n[0], n[1])<<n[2];
	fprintf (out, " %s%#x", cyel, num);
}

/*
 * Registers
 */

#define R(x) atomreg, x
void atomreg APROTO {
	fprintf (out, " %s$%s", cbl, (char *)v);
}
#define RA R("a")	// The scratch register. 0x40031c
#define RG R("g")	// PGRAPH offset register [?]. 0x400338
#define RR R("r")	// RAMIN offset register. 0x400334
#define RM R("m")	// ??? 0x40033c, writing from ctxprog aligns to 8.

/*
 * Memory fields
 */

// BF, offset shift, 'l'

int pgmem4[] = { 0, 14, 2, 'G' };
int pgmem5[] = { 0, 16, 2, 'G' };

#define PGRAPH4 atommem, pgmem4
#define PGRAPH5 atommem, pgmem5
void atommem APROTO {
	const int *n = v;
	fprintf (out, " %s%c[", ccy, n[3]);
//	if (n[3] == 'G') printf("%s$g%s+", cbl, ccy);
	int mo = BF(n[0], n[1])<<n[2];
	fprintf (out, "%s%#x%s]", cyel, mo, ccy);
}

/*
 * The flags, used as predicates for jump and wait insns.
 *
 * There are 0x80 of them, mapping directly to bits in PGRAPH regs 0x824
 * through 0x830. Known ones:
 *
 *  - 0x824 bit 0 [0x00]: direction flag used by pgraph opcode to select
 *    transfer direction.
 *  - 0x824 and 0x828, remaining bits [0x01-0x3f]: ??? RW
 *  - 0x82c [0x40-0x5f]: RO, some sort of PGRAPH status or something...
 *  - 0x82c bit 13 [0x4d]: always set, used for unconditional jumps
 *  - 0x830 bits 0-10 [0x60-0x69]: copied straight from relevant bits in PMC
 *    0x1540, the TP enable bits.
 *
 * None of this checked on NV40.
 */
struct insn tabrpred[] = {
	{ NVxx, 0x00, 0x7f, N("dir") }, // the direction flag
	{ NV5x, 0x4d, 0x7f },	// always?
	{ NV5x, 0x60, 0x60, N("unit"), UNIT }, // if given unit present
	{ NVxx, 0x00, 0x00, N("flag"), FLAG },
};

struct insn tabpred[] = {
	{ NVxx, 0x80, 0x80, N("not"), T(rpred) },
	{ NVxx, 0x00, 0x80, T(rpred) },
};

struct insn tabm[] = {
	{ NV5x, 0x100000, 0xff0000, N("pgraph"), PGRAPH5, RA },
	{ NV5x, 0x100000, 0xf00000, N("pgraph"), PGRAPH5, GSIZE5 },
	{ NV4x, 0x100000, 0xffc000, N("pgraph"), PGRAPH4, RA },
	{ NV4x, 0x100000, 0xf00000, N("pgraph"), PGRAPH4, GSIZE4 },
	{ NVxx, 0x200000, 0xf00000, N("mov"), RA, IMM },		// moves 20-bit immediate to scratch reg
	{ NV5x, 0x300000, 0xff0000, N("mov"), RG, GOFF },		// moves 20-bit immediate to $g reg
	{ NVxx, 0x400000, 0xfc0000, N("jmp"), T(pred), CTARG },		// jumps if condition true
	{ NV5x, 0x440000, 0xfc0000, N("call"), T(pred), CTARG },	// calls if condition true, NVAx only
	{ NV5x, 0x480000, 0xfc0000, N("ret"), T(pred) },		// rets if condition true, NVAx only
	{ NV5x, 0x500000, 0xf00000, N("wait"), T(pred) },		// waits until condition true.
	{ NV5x, 0x600006, 0xffffff, N("mov"), RR, RA },			// copies $a to $r
	{ NV5x, 0x600007, 0xffffff, N("mov"), RM, RA },			// copies $a to $r, anding it with 0xffff8
	{ NV5x, 0x60000c, 0xffffff, N("exit") },			// halts program execution, resets PC to 0
	{ NV4x, 0x60000e, 0xffffff, N("exit") },
	{ NVxx, 0x700000, 0xf00080, N("clear"), T(rpred) },		// clears given flag
	{ NVxx, 0x700080, 0xf00080, N("set"), T(rpred) },		// sets given flag
	{ NVxx, 0, 0, OOPS },
};

/*
 * Disassembler driver
 *
 * You pass a block of memory to this function, disassembly goes out to given
 * FILE*.
 */

void nv50dis (FILE *out, uint32_t *code, int num, int ptype) {
	int cur = 0;
	while (cur < num) {
		ull a = code[cur], m = 0;
		fprintf (out, "%s%08x: %s", cgray, cur*4, cnorm);
		fprintf (out, "%08llx", a);
		atomtab (out, &a, &m, tabm, ptype);
		a &= ~m;
		if (a) {
			fprintf (out, " %s[unknown: %08llx]%s", cred, a, cnorm);
		}
		printf ("%s\n", cnorm);
		cur++;
	}
}

int main(int argc, char **argv) {
	int ptype = NV5x;
	char c;
	while ((c = getopt (argc, argv, "45")) != -1)
		switch (c) {
			case '4':
				ptype = NV4x;
				break;
			case '5':
				ptype = NV5x;
				break;
		}
	int num = 0;
	int maxnum = 16;
	uint32_t *code = malloc (maxnum * 4);
	uint32_t t;
	while (!feof(stdin) && scanf ("%x", &t) == 1) {
		if (num == maxnum) maxnum *= 2, code = realloc (code, maxnum*4);
		code[num++] = t;
		scanf (" ,");
	}
	nv50dis (stdout, code, num, ptype);
	return 0;
}
