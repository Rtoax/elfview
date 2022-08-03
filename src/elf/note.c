// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2022 Rong Tao */
#include <stdint.h>
#include <stdio.h>
#include <libelf.h>
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>
#include <byteswap.h>
#include <endian.h>
#include <malloc.h>
#include <stdlib.h>
#include <assert.h>

#if defined(HAVE_ELFUTILS_DEVEL)
#include <elfutils/elf-knowledge.h>
#else
#define ELF_NOTE_GNU_BUILD_ATTRIBUTE_PREFIX "GA"
#define NT_GNU_BUILD_ATTRIBUTE_OPEN 0x100
#define NT_GNU_BUILD_ATTRIBUTE_FUNC 0x101
#endif

#include <elf/elf_api.h>
#include <utils/util.h>
#include <utils/log.h>

#if defined(__aarch64__) || defined(__x86_64__)
/* AArch64 specific GNU properties.
 * see elfutils/libelf/elf.h or linux/elf.h */
# ifndef GNU_PROPERTY_AARCH64_FEATURE_1_AND
#  define GNU_PROPERTY_AARCH64_FEATURE_1_AND  0xc0000000
# endif

# ifndef GNU_PROPERTY_AARCH64_FEATURE_1_BTI
#  define GNU_PROPERTY_AARCH64_FEATURE_1_BTI  (1U << 0)
# endif
# ifndef GNU_PROPERTY_AARCH64_FEATURE_1_PAC
#  define GNU_PROPERTY_AARCH64_FEATURE_1_PAC  (1U << 1)
# endif
#endif

/* Packaging metadata as defined on
 * https://systemd.io/COREDUMP_PACKAGE_METADATA/ */
#ifndef NT_FDO_PACKAGING_METADATA
#define NT_FDO_PACKAGING_METADATA 0xcafe1a7e
#endif

const char *n_type_core_string(GElf_Nhdr *nhdr)
{
	const char *res = NULL;

	static const char *knowntypes[] = {
#define KNOWNSTYPE(name) [NT_##name] = #name
	KNOWNSTYPE (PRSTATUS),
	KNOWNSTYPE (FPREGSET),
	KNOWNSTYPE (PRPSINFO),
	KNOWNSTYPE (TASKSTRUCT),
	KNOWNSTYPE (PLATFORM),
	KNOWNSTYPE (AUXV),
	KNOWNSTYPE (GWINDOWS),
	KNOWNSTYPE (ASRS),
	KNOWNSTYPE (PSTATUS),
	KNOWNSTYPE (PSINFO),
	KNOWNSTYPE (PRCRED),
	KNOWNSTYPE (UTSNAME),
	KNOWNSTYPE (LWPSTATUS),
	KNOWNSTYPE (LWPSINFO),
	KNOWNSTYPE (PRFPXREG)
#undef KNOWNSTYPE
	};
	if (nhdr->n_type < ARRAY_SIZE(knowntypes)
		&& knowntypes[nhdr->n_type] != NULL) {
		res = knowntypes[nhdr->n_type];
	} else {
		switch (nhdr->n_type) {
#define KNOWNSTYPE(name) case NT_##name: res = #name; break
		KNOWNSTYPE (PRXFPREG);
		KNOWNSTYPE (PPC_VMX);
		KNOWNSTYPE (PPC_SPE);
		KNOWNSTYPE (PPC_VSX);
		KNOWNSTYPE (PPC_TM_SPR);
		KNOWNSTYPE (386_TLS);
		KNOWNSTYPE (386_IOPERM);
		KNOWNSTYPE (X86_XSTATE);
		KNOWNSTYPE (S390_HIGH_GPRS);
		KNOWNSTYPE (S390_TIMER);
		KNOWNSTYPE (S390_TODCMP);
		KNOWNSTYPE (S390_TODPREG);
		KNOWNSTYPE (S390_CTRS);
		KNOWNSTYPE (S390_PREFIX);
		KNOWNSTYPE (S390_LAST_BREAK);
		KNOWNSTYPE (S390_SYSTEM_CALL);
		KNOWNSTYPE (ARM_VFP);
		KNOWNSTYPE (ARM_TLS);
		KNOWNSTYPE (ARM_HW_BREAK);
		KNOWNSTYPE (ARM_HW_WATCH);
		KNOWNSTYPE (ARM_SYSTEM_CALL);
		KNOWNSTYPE (SIGINFO);
		KNOWNSTYPE (FILE);
#undef KNOWNSTYPE

		default:
		res = "<unknown>";
		}
	}

	return res;
}

/* Don't output
 */
#define printf(...) \
	({int __ret = 0; __ret;})


const char *n_type_object_string(GElf_Nhdr *nhdr, const char *name,
	uint32_t type, GElf_Word descsz, char *buf, size_t len)
{
	const char *res = NULL;

	if (strcmp(name, "stapsdt") == 0) {
		snprintf(buf, len, "Version: %" PRIu32, type);
		return buf;
	}

#if defined(GO_NOTE_TYPE_H)
	// TODO: Go support?!
	static const char *goknowntypes[] = {
#define KNOWNSTYPE(name) [ELF_NOTE_GO##name] = #name
	KNOWNSTYPE (PKGLIST),
	KNOWNSTYPE (ABIHASH),
	KNOWNSTYPE (DEPS),
	KNOWNSTYPE (BUILDID),
	NULL,
#undef KNOWNSTYPE
	};
	if (strcmp(name, "Go") == 0) {
		if (type < ARRAY_SIZE(goknowntypes)
			&& goknowntypes[type] != NULL)
			return goknowntypes[type];
		else {
			snprintf(buf, len, "%s: %" PRIu32 "<unknown>", type);
			return buf;
		}
	}
#endif

	// name has "GA" prefix, for example:
	// $ strings /bin/ls | grep  GA
	//  GA+GLIBCXX_ASSERTIONS
	//  GA*cf_protection
	//  GA+omit_frame_pointer
	//  GA+stack_clash
	//  GA*FORTIFY
	//  GA*GOW
	//  GA*FORTIFY
	//  GA+GLIBCXX_ASSERTIONS
	//  GA*GOW
	//  GA*cf_protection
	//  GA+omit_frame_pointer
	//  GA+stack_clash
	//  GA*FORTIFY
	//  GA*GOW
	//  GA!stack_realign
	//  GA*FORTIFY
	//  GA+GLIBCXX_ASSERTIONS
	//  GA+GLIBCXX_ASSERTIONS
	//  GA*FORTIFY
	if (startswith(name, ELF_NOTE_GNU_BUILD_ATTRIBUTE_PREFIX)) {

		/* GNU Build Attribute notes (ab)use the owner name to store
		 * most of their data.  Don't decode everything here.  Just
		 * the type.*/
		char *t = buf;
		const char *gba = "GNU Build Attribute";
		int w = snprintf(t, len, "%s ", gba);
		t += w;
		len -= w;

		if (type == NT_GNU_BUILD_ATTRIBUTE_OPEN)
			snprintf(t, len, "OPEN");
		else if (type == NT_GNU_BUILD_ATTRIBUTE_FUNC)
			snprintf(t, len, "FUNC");
		else
			snprintf(t, len, "%x", type);

		return buf;
	}

	if (strcmp(name, "FDO") == 0 && type == NT_FDO_PACKAGING_METADATA)
		return "FDO_PACKAGING_METADATA";

	if (strcmp(name, "GNU") != 0) {

		/* NT_VERSION is special, all data is in the name.  */
		if (descsz == 0 && type == NT_VERSION)
			return "VERSION";

		snprintf(buf, len, "%s: %" PRIu32, "<unknown>", type);
		return buf;
	}

	/* And finally all the "GNU" note types.  */
	static const char *knowntypes[] = {
#define KNOWNSTYPE(name) [NT_##name] = #name
		KNOWNSTYPE (GNU_ABI_TAG),
		KNOWNSTYPE (GNU_HWCAP),
		KNOWNSTYPE (GNU_BUILD_ID),
		KNOWNSTYPE (GNU_GOLD_VERSION),
		KNOWNSTYPE (GNU_PROPERTY_TYPE_0),
#undef KNOWNSTYPE
	};

	/* Handle standard names.  */
	if (type < ARRAY_SIZE(knowntypes) && knowntypes[type] != NULL)
		res = knowntypes[type];

	else {
		snprintf(buf, len, "%s: %" PRIu32, "<unknown>", type);
		res = buf;
	}

	return res;
}

static void handle_auxv_note(struct elf_file *elf, GElf_Word descsz,
	GElf_Off desc_pos)
{
	Elf_Data *data = elf_getdata_rawchunk(elf->elf,
		desc_pos, descsz, ELF_T_AUXV);
	if (data == NULL) {
elf_error:
		lerror("cannot convert core note data: %s", elf_errmsg(-1));
		return;
	}

	size_t i;
	const size_t nauxv =
		descsz / gelf_fsize(elf->elf, ELF_T_AUXV, 1, EV_CURRENT);

	for (i = 0; i < nauxv; ++i) {
		GElf_auxv_t av_mem;
		GElf_auxv_t *av = gelf_getauxv(data, i, &av_mem);
		if (av == NULL)
			goto elf_error;

		const char *name;
		const char *fmt;

		if (auxv_type_info(av->a_type, &name, &fmt) == 0) {
			/* Unknown type.  */
			if (av->a_un.a_val == 0)
				printf("    %" PRIu64 "\n", av->a_type);
			else
				printf("    %" PRIu64 ": %#" PRIx64 "\n",
					av->a_type, av->a_un.a_val);
		} else {

			switch (fmt[0]) {
			case '\0': /* Normally zero. */
				if (av->a_un.a_val == 0) {
					printf("    %s\n", name);
					break;
				}
			FALLTHROUGH;
			case 'x':		/* hex */
			case 'p':		/* address */
			case 's':		/* address of string */
				printf("    %s: %#" PRIx64 "\n", name, av->a_un.a_val);
				break;
			case 'u':
				printf("    %s: %" PRIu64 "\n", name, av->a_un.a_val);
				break;
			case 'd':
				printf("    %s: %" PRId64 "\n", name, av->a_un.a_val);
				break;

			case 'b':
				printf("    %s: %#" PRIx64 "  ", name, av->a_un.a_val);
				GElf_Xword bit = 1;
				const char __unused *pfx = "<";
				const char *p = NULL;
				for (p = fmt + 1; *p != 0; p = strchr(p, '\0') + 1) {
					if (av->a_un.a_val & bit) {
						printf("%s%s", pfx, p);
						pfx = " ";
					}
					bit <<= 1;
				}
				printf(">\n");
				break;

			default:
				abort();
			}
		}
	}
}

static const void *
convert(Elf *core, Elf_Type type, uint_fast16_t count,
	void *value, const void *data, size_t size)
{
	Elf_Data valuedata = {
		.d_type = type,
		.d_buf = value,
		.d_size = size ?: gelf_fsize (core, type, count, EV_CURRENT),
		.d_version = EV_CURRENT,
	};

	Elf_Data indata = {
		.d_type = type,
		.d_buf = (void *) data,
		.d_size = valuedata.d_size,
		.d_version = EV_CURRENT,
	};

	// not support 32bit yet
	Elf_Data *d = (gelf_getclass(core) == ELFCLASS32
		? elf32_xlatetom : elf64_xlatetom)
			(&valuedata, &indata, elf_getident (core, NULL)[EI_DATA]);
	if (d == NULL) {
		lerror("cannot convert core note data: %s", elf_errmsg(-1));
		return 0;
	}

	return data + indata.d_size;
}

typedef uint8_t GElf_Byte;

static bool
buf_has_data(unsigned char const *ptr, unsigned char const *end, size_t sz)
{
	return ptr < end && (size_t) (end - ptr) >= sz;
}

static bool
buf_read_int(Elf *core, unsigned char const **ptrp, unsigned char const *end,
	int *retp)
{
	if (! buf_has_data(*ptrp, end, 4))
		return false;

	*ptrp = convert (core, ELF_T_WORD, 1, retp, *ptrp, 4);
	return true;
}

static bool
buf_read_ulong(Elf *core, unsigned char const **ptrp, unsigned char const *end,
	uint64_t *retp)
{
	size_t sz = gelf_fsize(core, ELF_T_ADDR, 1, EV_CURRENT);
	if (! buf_has_data (*ptrp, end, sz))
		return false;

	union {
		uint64_t u64;
		uint32_t u32;
	} u;

	*ptrp = convert(core, ELF_T_ADDR, 1, &u, *ptrp, sz);

	if (sz == 4)
		*retp = u.u32;
	else
		*retp = u.u64;
	return true;
}

static void __unused
handle_siginfo_note(struct elf_file *elf, GElf_Word descsz, GElf_Off desc_pos)
{
	Elf *core = elf->elf;
	Elf_Data *data = elf_getdata_rawchunk (core, desc_pos, descsz, ELF_T_BYTE);
	if (data == NULL) {
		lerror("cannot convert core note data: %s", elf_errmsg (-1));
		return;
	}

	unsigned char const *ptr = data->d_buf;
	unsigned char const *const end = data->d_buf + data->d_size;

	/* Siginfo head is three ints: signal number, error number, origin
	 * code. */
	int si_signo, si_errno, si_code;
	if (! buf_read_int (core, &ptr, end, &si_signo)
		|| ! buf_read_int (core, &ptr, end, &si_errno)
		|| ! buf_read_int (core, &ptr, end, &si_code))
	{
fail:
		printf("    Not enough data in NT_SIGINFO note.\n");
		return;
	}

	/* Next is a pointer-aligned union of structures.  On 64-bit
	 * machines, that implies a word of padding.  */
	if (gelf_getclass(core) == ELFCLASS64)
		ptr += 4;

	printf("    si_signo: %d, si_errno: %d, si_code: %d\n",
		si_signo, si_errno, si_code);

	if (si_code > 0) {
		switch (si_signo) {
		case CORE_SIGILL:
		case CORE_SIGFPE:
		case CORE_SIGSEGV:
		case CORE_SIGBUS:
		{
			uint64_t addr;
			if (! buf_read_ulong (core, &ptr, end, &addr))
				goto fail;
			printf("    fault address: %#" PRIx64 "\n", addr);
			break;
		}
		default:
			;
		}

	} else if (si_code == CORE_SI_USER) {
		int pid, uid;
		if (! buf_read_int (core, &ptr, end, &pid)
			|| ! buf_read_int (core, &ptr, end, &uid))
			goto fail;
		printf("    sender PID: %d, sender UID: %d\n", pid, uid);
	}
}

static void __unused
handle_file_note (struct elf_file *elf, GElf_Word descsz, GElf_Off desc_pos)
{
	Elf *core = elf->elf;
	Elf_Data *data = elf_getdata_rawchunk (core, desc_pos, descsz, ELF_T_BYTE);
	if (data == NULL) {
		lerror("cannot convert core note data: %s", elf_errmsg (-1));
	}

	unsigned char const *ptr = data->d_buf;
	unsigned char const *const end = data->d_buf + data->d_size;

	uint64_t count, page_size;
	if (! buf_read_ulong (core, &ptr, end, &count)
		|| ! buf_read_ulong (core, &ptr, end, &page_size))
	{
fail:
		printf("    Not enough data in NT_FILE note.\n");
		return;
	}

	size_t addrsize = gelf_fsize (core, ELF_T_ADDR, 1, EV_CURRENT);
	uint64_t maxcount = (size_t) (end - ptr) / (3 * addrsize);
	if (count > maxcount)
		goto fail;

	/* Where file names are stored.  */
	unsigned char const *const fstart = ptr + 3 * count * addrsize;
	char const *fptr = (char *) fstart;

	printf("    %" PRId64 " files:\n", count);
	for (uint64_t i = 0; i < count; ++i) {
		uint64_t mstart, mend, moffset;
		if (! buf_read_ulong (core, &ptr, fstart, &mstart)
		  || ! buf_read_ulong (core, &ptr, fstart, &mend)
		  || ! buf_read_ulong (core, &ptr, fstart, &moffset))
		{
			goto fail;
		}

		const char *fnext = memchr (fptr, '\0', (char *) end - fptr);
		if (fnext == NULL)
			goto fail;

		int __unused ct = printf("      %08" PRIx64 "-%08" PRIx64
	       " %08" PRIx64 " %" PRId64,
	       mstart, mend, moffset * page_size, mend - mstart);
		printf("%*s%s\n", ct > 50 ? 3 : 53 - ct, "", fptr);

		fptr = fnext + 1;
	}
}

/* Align offset to 4 bytes as needed for note name and descriptor data.
 * This is almost always used, except for GNU Property notes, which use
 * 8 byte padding...  */
#define NOTE_ALIGN4(n)	(((n) + 3) & -4UL)

/* Special note padding rule for GNU Property notes.  */
#define NOTE_ALIGN8(n)	(((n) + 7) & -8UL)

static void __unused
elf_object_note(struct elf_file *elf, uint32_t namesz, const char *name,
	uint32_t type, uint32_t descsz, const char *desc)
{
	/* The machine specific function did not know this type.  */

	if (strcmp ("stapsdt", name) == 0) {
		if (type != 3) {
			lerror("unknown SDT version %u\n", type);
			return;
		}

		/* Descriptor starts with three addresses, pc, base ref and
		 * semaphore.  Then three zero terminated strings provider,
		 * name and arguments.  */
		union {
			Elf64_Addr a64[3];
			Elf32_Addr a32[3];
		} addrs;

		size_t addrs_size = gelf_fsize(elf->elf, ELF_T_ADDR, 3, EV_CURRENT);
		if (descsz < addrs_size + 3) {
invalid_sdt:
			lerror("invalid SDT probe descriptor\n");
			return;
		}

		Elf_Data src = {
			.d_type = ELF_T_ADDR,
			.d_version = EV_CURRENT,
			.d_buf = (void *) desc,
			.d_size = addrs_size
	    };
		Elf_Data dst = {
			.d_type = ELF_T_ADDR,
			.d_version = EV_CURRENT,
			.d_buf = &addrs,
			.d_size = addrs_size
	    };

		if (gelf_xlatetom(elf->elf, &dst, &src,
				elf_getident(elf->elf, NULL)[EI_DATA]) == NULL)
		{
			lerror("%s\n", elf_errmsg(-1));
			return;
	    }

		const char *provider = desc + addrs_size;
		const char *pname = memchr(provider, '\0', desc + descsz - provider);
		if (pname == NULL)
			goto invalid_sdt;

		++pname;
		const char *args = memchr(pname, '\0', desc + descsz - pname);
		if (args == NULL ||
			memchr(++args, '\0', desc + descsz - pname) != desc + descsz - 1)
		{
			goto invalid_sdt;
		}

		GElf_Addr __unused pc;
		GElf_Addr __unused base;
		GElf_Addr __unused sem;

		// already not support 32bit when open elf file
		if (gelf_getclass(elf->elf) == ELFCLASS32) {
			pc = addrs.a32[0];
			base = addrs.a32[1];
			sem = addrs.a32[2];
		} else {
			pc = addrs.a64[0];
			base = addrs.a64[1];
			sem = addrs.a64[2];
	    }

		printf("    PC: ");
		printf("%#" PRIx64 ",", pc);
		printf(" Base: ");
		printf("%#" PRIx64 ",", base);
		printf(" Semaphore: ");
		printf("%#" PRIx64 "\n", sem);
		printf("    Provider: ");
		printf("%s,", provider);
		printf(" Name: ");
		printf("%s,", pname);
		printf(" Args: ");
		printf("'%s'\n", args);
		return;
	} // stapsdt


	// name has "GA" prefix
	if (strncmp(name, ELF_NOTE_GNU_BUILD_ATTRIBUTE_PREFIX,
			strlen(ELF_NOTE_GNU_BUILD_ATTRIBUTE_PREFIX)) == 0
		&& (type == NT_GNU_BUILD_ATTRIBUTE_OPEN
			|| type == NT_GNU_BUILD_ATTRIBUTE_FUNC))
	{
		/* There might or might not be a pair of addresses in the desc.  */
		if (descsz > 0) {
			printf("    Address Range: ");

			union {
				Elf64_Addr a64[2];
				Elf32_Addr a32[2];
			} addrs;

			size_t addr_size = gelf_fsize(elf->elf, ELF_T_ADDR, 2, EV_CURRENT);
			if (descsz != addr_size)
				lerror("<unknown data>\n");
			else
			{
				Elf_Data src = {
					.d_type = ELF_T_ADDR,
					.d_version = EV_CURRENT,
					.d_buf = (void *) desc,
					.d_size = descsz
				};

				Elf_Data dst = {
					.d_type = ELF_T_ADDR,
					.d_version = EV_CURRENT,
					.d_buf = &addrs,
					.d_size = descsz
				};

				if (gelf_xlatetom(elf->elf, &dst, &src,
						elf_getident(elf->elf, NULL)[EI_DATA]) == NULL)
					lerror("%s\n", elf_errmsg(-1));
				else
				{
					// 32bit
					if (addr_size == 4)
						printf("%#" PRIx32 " - %#" PRIx32 "\n",
							addrs.a32[0], addrs.a32[1]);
					else
						printf("%#" PRIx64 " - %#" PRIx64 "\n",
							addrs.a64[0], addrs.a64[1]);
				}
			}
		}

		/* Most data actually is inside the name.
		 * https://fedoraproject.org/wiki/Toolchain/Watermark  */

		/* We need at least 2 chars of data to describe the
		 * attribute and value encodings.
		 *
		 * 'name' has "GA" prefix, skip "GA" */
		const char *data = (name
				+ strlen(ELF_NOTE_GNU_BUILD_ATTRIBUTE_PREFIX));
		if (namesz < 2) {
			lerror("<insufficient data>\n");
			return;
		}

		printf("    ");

		/* In most cases the value comes right after the encoding bytes.  */
		const char *value = &data[2];
		switch (data[1]) {
		case GNU_BUILD_ATTRIBUTE_VERSION:
			printf("VERSION: ");
			break;
		case GNU_BUILD_ATTRIBUTE_STACK_PROT:
			printf("STACK_PROT: ");
			break;
		case GNU_BUILD_ATTRIBUTE_RELRO:
			printf("RELRO: ");
			break;
		case GNU_BUILD_ATTRIBUTE_STACK_SIZE:
			printf("STACK_SIZE: ");
			break;
		case GNU_BUILD_ATTRIBUTE_TOOL:
			printf("TOOL: ");
			break;
		case GNU_BUILD_ATTRIBUTE_ABI:
			printf("ABI: ");
			break;
		case GNU_BUILD_ATTRIBUTE_PIC:
			printf("PIC: ");
			break;
		case GNU_BUILD_ATTRIBUTE_SHORT_ENUM:
			printf("SHORT_ENUM: ");
			break;
		case 32 ... 126:
			printf("\"%s\": ", &data[1]);
			value += strlen(&data[1]) + 1;
			break;
		default:
			lerror("<unknown>: ");
			break;
	    }

		switch (data[0]) {
		case GNU_BUILD_ATTRIBUTE_TYPE_NUMERIC:
		{
			/* Any numbers are always in (unsigned) little endian.  */
			bool other_byte_order =
				(elf->ehdr->e_ident[EI_DATA] != ELFDATA2LSB);
			size_t bytes = namesz - (value - name);
			uint64_t __unused val;
# define read_2ubyte_unaligned(order, Addr) \
	(unlikely(order)	\
	 ? bswap_16 (*((const uint16_t *) (Addr)))	\
	 : *((const uint16_t *) (Addr)))
# define read_4ubyte_unaligned(order, Addr) \
	(unlikely(order)	\
	 ? bswap_32 (*((const uint32_t *) (Addr)))	\
	 : *((const uint32_t *) (Addr)))
# define read_8ubyte_unaligned(order, Addr) \
	(unlikely(order)	\
	 ? bswap_64 (*((const uint64_t *) (Addr)))	\
	 : *((const uint64_t *) (Addr)))

			if (bytes == 1)
				val = *(unsigned char *) value;
			else if (bytes == 2)
				val = read_2ubyte_unaligned (other_byte_order, value);
			else if (bytes == 4)
				val = read_4ubyte_unaligned (other_byte_order, value);
			else if (bytes == 8)
				val = read_8ubyte_unaligned (other_byte_order, value);
			else
				goto unknown;
			printf("%" PRIx64, val);
		}
			break;
		case GNU_BUILD_ATTRIBUTE_TYPE_STRING:
			printf("\"%s\"", value);
			break;
		case GNU_BUILD_ATTRIBUTE_TYPE_BOOL_TRUE:
			printf("TRUE");
			break;
		case GNU_BUILD_ATTRIBUTE_TYPE_BOOL_FALSE:
			printf("FALSE");
			break;
		default:
		{
unknown:
			printf("<unknown>");
		}
			break;
		}

		printf("\n");

		return;
	}

	/* NT_VERSION doesn't have any info.  All data is in the name.  */
	if (descsz == 0 && type == NT_VERSION)
		return;

	if (strcmp("FDO", name) == 0
		&& type == NT_FDO_PACKAGING_METADATA
		&& descsz > 0 && desc[descsz - 1] == '\0')
	{
		printf("    Packaging Metadata: %.*s\n", (int) descsz, desc);
	}

	/* Everything else should have the "GNU" owner name.  */
	if (strcmp("GNU", name) != 0)
		return;

	switch (type) {
	case NT_GNU_BUILD_ID:
		if (strcmp(name, "GNU") == 0 && descsz > 0) {
			printf("    Build ID: ");
			uint_fast32_t i;
			char v[3] = {};
			char *build_id = malloc(descsz * 2 + 1);
			assert(build_id && "Malloc fatal.");

			// save Build ID, see:
			// $ readelf -n /bin/ls | grep "Build ID"
			//  Build ID: 49c2fad65d0c2df70025644c9bc7485b28bab899

			for (i = 0; i < descsz - 1; ++i) {
				printf("%02" PRIx8, (uint8_t) desc[i]);
				sprintf(v, "%02" PRIx8, (uint8_t) desc[i]);
				build_id[i * 2] = v[0];
				build_id[i * 2 + 1] = v[1];
			}
			printf("%02" PRIx8 "\n", (uint8_t) desc[i]);
			sprintf(v, "%02" PRIx8, (uint8_t) desc[i]);
			build_id[i * 2] = v[0];
			build_id[i * 2 + 1] = v[1];
			build_id[i * 2 + 2] = '\0';

			ldebug("Build ID: %s\n", build_id);

			elf->build_id = build_id;
		}
		break;

	case NT_GNU_GOLD_VERSION:
		if (strcmp(name, "GNU") == 0 && descsz > 0)
			/* A non-null terminated version string.  */
			printf("    Linker version: %.*s\n", (int) descsz, desc);
		break;

	case NT_GNU_PROPERTY_TYPE_0:
	if (strcmp(name, "GNU") == 0 && descsz > 0) {
		/* There are at least 2 words. type and datasz.  */
		while (descsz >= 8) {
			struct pr_prop {
				GElf_Word pr_type;
				GElf_Word pr_datasz;
			} prop;

			Elf_Data in = {
				.d_version = EV_CURRENT,
				.d_type = ELF_T_WORD,
				.d_size = 8,
				.d_buf = (void *) desc
			};
			Elf_Data out = {
				.d_version = EV_CURRENT,
				.d_type = ELF_T_WORD,
				.d_size = descsz,
				.d_buf = (void *) &prop
			};

			if (gelf_xlatetom(elf->elf, &out, &in,
					elf_getident (elf->elf, NULL)[EI_DATA]) == NULL)
			{
				printf("%s\n", elf_errmsg (-1));
				return;
			}

			desc += 8;
			descsz -= 8;

			if (prop.pr_datasz > descsz) {
				lerror("BAD property datasz: %" PRId32 "\n", prop.pr_datasz);
				return;
			}

			int elfclass = gelf_getclass (elf->elf);
			char *elfident = elf_getident (elf->elf, NULL);
			GElf_Ehdr ehdr;
			gelf_getehdr (elf->elf, &ehdr);

			/* Prefix.  */
			printf("    ");
			if (prop.pr_type == GNU_PROPERTY_STACK_SIZE) {
				printf("STACK_SIZE ");
				union {
					Elf64_Addr a64;
					Elf32_Addr a32;
				} addr;
				if ((elfclass == ELFCLASS32 && prop.pr_datasz == 4)
					|| (elfclass == ELFCLASS64 && prop.pr_datasz == 8))
				{
					in.d_type = ELF_T_ADDR;
					out.d_type = ELF_T_ADDR;
					in.d_size = prop.pr_datasz;
					out.d_size = prop.pr_datasz;
					in.d_buf = (void *) desc;
					out.d_buf = (elfclass == ELFCLASS32
						? (void *) &addr.a32
						: (void *) &addr.a64);

					if (gelf_xlatetom(elf->elf, &out, &in,
							elfident[EI_DATA]) == NULL)
					{
						printf("%s\n", elf_errmsg (-1));
						return;
					}
					if (elfclass == ELFCLASS32)
						printf("%#" PRIx32 "\n", addr.a32);
					else
						printf("%#" PRIx64 "\n", addr.a64);
				} else
					printf(" (garbage datasz: %" PRIx32 ")\n", prop.pr_datasz);

			} else if (prop.pr_type == GNU_PROPERTY_NO_COPY_ON_PROTECTED) {

				printf("NO_COPY_ON_PROTECTION");
				if (prop.pr_datasz == 0)
					printf("\n");
				else
					printf(" (garbage datasz: %" PRIx32 ")\n",
						prop.pr_datasz);

			} else if (prop.pr_type >= GNU_PROPERTY_LOPROC
					&& prop.pr_type <= GNU_PROPERTY_HIPROC
					&& (ehdr.e_machine == EM_386
						|| ehdr.e_machine == EM_X86_64))
			{
				printf("X86 ");

				if (prop.pr_type == GNU_PROPERTY_X86_FEATURE_1_AND) {
					printf("FEATURE_1_AND: ");

					if (prop.pr_datasz == 4) {
						GElf_Word data;
						in.d_type = ELF_T_WORD;
						out.d_type = ELF_T_WORD;
						in.d_size = 4;
						out.d_size = 4;
						in.d_buf = (void *) desc;
						out.d_buf = (void *) &data;

						if (gelf_xlatetom(elf->elf, &out, &in,
								elfident[EI_DATA]) == NULL)
						{
							lerror("%s\n", elf_errmsg (-1));
							return;
						}
						printf("%08" PRIx32 " ", data);

						if ((data & GNU_PROPERTY_X86_FEATURE_1_IBT) != 0) {
							printf("IBT");
							data &= ~GNU_PROPERTY_X86_FEATURE_1_IBT;
							if (data != 0)
								printf(" ");
						}

						if ((data & GNU_PROPERTY_X86_FEATURE_1_SHSTK) != 0) {
							printf("SHSTK");
							data &= ~GNU_PROPERTY_X86_FEATURE_1_SHSTK;
							if (data != 0)
								printf(" ");
						}

						if (data != 0)
							printf("UNKNOWN");

					} else {
						printf("<bad datasz: %" PRId32 ">", prop.pr_datasz);
					}

					printf("\n");

				} else {

					printf("%#" PRIx32, prop.pr_type);
					if (prop.pr_datasz > 0) {
						printf(" data: ");
						size_t i;
						for (i = 0; i < prop.pr_datasz - 1; i++) {
							printf("%02" PRIx8 " ", (uint8_t) desc[i]);
						}
						printf("%02" PRIx8 "\n", (uint8_t) desc[i]);
					}
				}

			} else if (prop.pr_type >= GNU_PROPERTY_LOPROC
					&& prop.pr_type <= GNU_PROPERTY_HIPROC
					&& ehdr.e_machine == EM_AARCH64)
			{
				printf("AARCH64 ");
				if (prop.pr_type == GNU_PROPERTY_AARCH64_FEATURE_1_AND) {
					printf("FEATURE_1_AND: ");

					if (prop.pr_datasz == 4) {
						GElf_Word data;
						in.d_type = ELF_T_WORD;
						out.d_type = ELF_T_WORD;
						in.d_size = 4;
						out.d_size = 4;
						in.d_buf = (void *) desc;
						out.d_buf = (void *) &data;

						if (gelf_xlatetom(elf->elf, &out, &in,
								elfident[EI_DATA]) == NULL)
						{
							lerror("%s\n", elf_errmsg(-1));
							return;
						}
						printf("%08" PRIx32 " ", data);

						if ((data & GNU_PROPERTY_AARCH64_FEATURE_1_BTI) != 0) {
							printf("BTI");
							data &= ~GNU_PROPERTY_AARCH64_FEATURE_1_BTI;
							if (data != 0)
								printf(" ");
						}

						if ((data & GNU_PROPERTY_AARCH64_FEATURE_1_PAC) != 0) {
							printf("PAC");
							data &= ~GNU_PROPERTY_AARCH64_FEATURE_1_PAC;
							if (data != 0)
								printf(" ");
						}

						if (data != 0)
							printf("UNKNOWN");

					} else {
						printf("<bad datasz: %" PRId32 ">", prop.pr_datasz);
					}

					printf("\n");

				} else {
					printf("%#" PRIx32, prop.pr_type);

					if (prop.pr_datasz > 0) {
						printf(" data: ");
						size_t i;
						for (i = 0; i < prop.pr_datasz - 1; i++) {
							printf("%02" PRIx8 " ", (uint8_t) desc[i]);
						}
						printf("%02" PRIx8 "\n", (uint8_t) desc[i]);
					}
				}

			} else {

				if (prop.pr_type >= GNU_PROPERTY_LOPROC
					&& prop.pr_type <= GNU_PROPERTY_HIPROC)
				{
					printf("proc_type %#" PRIx32, prop.pr_type);
				} else if (prop.pr_type >= GNU_PROPERTY_LOUSER
					&& prop.pr_type <= GNU_PROPERTY_HIUSER)
				{
					printf("app_type %#" PRIx32, prop.pr_type);
				} else {
					printf("unknown_type %#" PRIx32, prop.pr_type);
				}

				if (prop.pr_datasz > 0) {
					printf(" data: ");
					size_t i;
					for (i = 0; i < prop.pr_datasz - 1; i++) {
						printf("%02" PRIx8 " ", (uint8_t) desc[i]);
					}
					printf("%02" PRIx8 "\n", (uint8_t) desc[i]);
				}
			}

			if (elfclass == ELFCLASS32)
				prop.pr_datasz = NOTE_ALIGN4 (prop.pr_datasz);
			else
				prop.pr_datasz = NOTE_ALIGN8 (prop.pr_datasz);

			desc += prop.pr_datasz;
			if (descsz > prop.pr_datasz)
				descsz -= prop.pr_datasz;
			else
				descsz = 0;
		}
	}
		break;

	case NT_GNU_ABI_TAG:
	if (descsz >= 8 && descsz % 4 == 0) {
		Elf_Data in = {
			.d_version = EV_CURRENT,
			.d_type = ELF_T_WORD,
			.d_size = descsz,
			.d_buf = (void *) desc
		};
		/* Normally NT_GNU_ABI_TAG is just 4 words (16 bytes).  If it
		 * is much (4*) larger dynamically allocate memory to convert. */
#define FIXED_TAG_BYTES 16
		uint32_t sbuf[FIXED_TAG_BYTES];
		uint32_t *buf;
	    if (unlikely(descsz / 4 > FIXED_TAG_BYTES)) {
			buf = malloc (descsz);
			if (unlikely (buf == NULL))
				return;

		} else
			buf = sbuf;

		Elf_Data out = {
			.d_version = EV_CURRENT,
			.d_type = ELF_T_WORD,
			.d_size = descsz,
			.d_buf = buf
		};

		if (elf32_xlatetom(&out, &in, elf->ehdr->e_ident[EI_DATA]) != NULL) {

			const char __unused *os;

			switch (buf[0]) {
			case ELF_NOTE_OS_LINUX:
				os = "Linux";
				break;

			case ELF_NOTE_OS_GNU:
				os = "GNU";
				break;

			case ELF_NOTE_OS_SOLARIS2:
				os = "Solaris";
				break;

			case ELF_NOTE_OS_FREEBSD:
				os = "FreeBSD";
				break;

			default:
				os = "???";
				break;
			}

			printf("    OS: %s, ABI: ", os);

			for (size_t cnt = 1; cnt < descsz / 4; ++cnt) {
				if (cnt > 1)
					putchar_unlocked('.');
				printf("%" PRIu32, buf[cnt]);
			}
			putchar_unlocked('\n');
		}

		if (descsz / 4 > FIXED_TAG_BYTES)
			free (buf);
	      break;
	}
	FALLTHROUGH;

	default:
		/* Unknown type.  */
		break;
	}
}

int handle_notes(struct elf_file *elf, GElf_Shdr *shdr, Elf_Scn *scn)
{
	Elf_Data *data = elf_getdata(scn, NULL);

	if (!data)
		goto bad_note;

	size_t offset  = 0;
	GElf_Nhdr nhdr;
	size_t name_offset;
	size_t desc_offset;

	while (offset < data->d_size
		&& (offset = gelf_getnote(data, offset,
			&nhdr, &name_offset, &desc_offset)) > 0)
	{
		const char *name = nhdr.n_namesz == 0 ? "" : data->d_buf + name_offset;
		const char __unused *desc = data->d_buf + desc_offset;

		/* GNU Build Attributes are weird, they store most of their data
		 * into the owner name field.  Extract just the owner name
		 * prefix here, then use the rest later as data. */
		bool is_gnu_build_attr =
			startswith(name, ELF_NOTE_GNU_BUILD_ATTRIBUTE_PREFIX);

		// if name has "GA" prefix
		const char *print_name = (is_gnu_build_attr
			? ELF_NOTE_GNU_BUILD_ATTRIBUTE_PREFIX : name);

		size_t __unused print_namesz = (is_gnu_build_attr
			? strlen (print_name) : nhdr.n_namesz);

		char __unused buf[100];
		printf("  %-13.*s  %9" PRId32 "  %s\n",
			(int) print_namesz, print_name, nhdr.n_descsz,
			elf->ehdr->e_type == ET_CORE
			? n_type_core_string(&nhdr)
			: n_type_object_string(&nhdr, name,
				nhdr.n_type, nhdr.n_descsz, buf, sizeof(buf)));
		/* Filter out invalid entries.  */
		if (memchr(name, '\0', nhdr.n_namesz) != NULL
			/* XXX For now help broken Linux kernels.  */
			|| 1)
		{
			if (elf->ehdr->e_type == ET_CORE) {
				if (nhdr.n_type == NT_AUXV
					&& (nhdr.n_namesz == 4 /* Broken old Linux kernels.  */
						|| (nhdr.n_namesz == 5 && name[4] == '\0'))
					&& !memcmp (name, "CORE", 4))
				{
					handle_auxv_note(elf, nhdr.n_descsz,
						shdr->sh_offset + desc_offset);
				} else if (nhdr.n_namesz == 5 && strcmp (name, "CORE") == 0) {

					switch (nhdr.n_type) {
					case NT_SIGINFO:
						handle_siginfo_note(elf, nhdr.n_descsz,
							shdr->sh_offset + desc_offset);
						break;
					case NT_FILE:
						handle_file_note(elf, nhdr.n_descsz,
							shdr->sh_offset + desc_offset);
						break;

#if 0
					default:
						handle_core_note(ebl, &nhdr, name, desc);
#endif
					}
				} else {
#if 0
					handle_core_note(ebl, &nhdr, name, desc);
#endif
				}
			} else {
				elf_object_note(elf, nhdr.n_namesz, name, nhdr.n_type,
					nhdr.n_descsz, desc);
			}
		}
	}
	if (offset == data->d_size)
		return 0;

bad_note:
	lerror("cannot get content of note: %s",
		data != NULL ? "garbage data" : elf_errmsg(-1));
	return -ENODATA;
}

