/* SPDX-License-Identifier: GPL-2.0 */
static const char *__doc__ = "XDP loader\n";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <net/if.h>
#include <linux/if_link.h> /* depend on kernel-headers installed */

#include "common_user.h"

static const char *default_filename = "xdp_prog_kern.o";
static const char *default_progname = "xdp_pass";

static const struct option long_options[] = {
	{"help",        no_argument,		NULL, 'h' },
	{"dev",         required_argument,	NULL, 'd' },
	{"skb-mode",    no_argument,		NULL, 'S' },
	{"native-mode", no_argument,		NULL, 'N' },
	{"auto-mode",   no_argument,		NULL, 'A' },
	{"force",       no_argument,		NULL, 'F' },
	{"unload",      no_argument,		NULL, 'U' },
	{"filename",    required_argument,	NULL,  1  },
	{"progname",    required_argument,	NULL,  2  },
	{0, 0, NULL,  0 }
};

struct bpf_object *load_bpf_object_file(const char *filename)
{
	int first_prog_fd = -1;
	struct bpf_object *obj;
	int err;

	struct bpf_prog_load_attr prog_load_attr = {
		.prog_type      = BPF_PROG_TYPE_XDP,
	};
	prog_load_attr.file = filename;

	/* Use libbpf for extracting BPF byte-code from BPF-ELF object, and
	 * loading this into the kernel via bpf-syscall
	 */
	err = bpf_prog_load_xattr(&prog_load_attr, &obj, &first_prog_fd);
	if (err) {
		fprintf(stderr, "ERR: loading BPF-OBJ file(%s) (%d): %s\n",
			filename, err, strerror(-err));
		return NULL;
	}

	return obj;
}

static int xdp_unload(int ifindex, __u32 xdp_flags)
{
	int err;

	if ((err = bpf_set_link_xdp_fd(ifindex, -1, xdp_flags)) < 0) {
		fprintf(stderr, "ERR: link set xdp unload failed (err=%d):%s\n",
			err, strerror(-err));
		return EXIT_FAIL_XDP;
	}
	return EXIT_OK;
}

static int xdp_load(struct config cfg, int prog_fd)
{
	int err;

	/* libbpf provide the XDP net_device link-level hook attach helper */
	err = bpf_set_link_xdp_fd(cfg.ifindex, prog_fd, cfg.xdp_flags);
	if (err < 0) {
		fprintf(stderr, "ERR: dev:%s link set xdp fd failed (%d): %s\n",
			cfg.ifname, -err, strerror(-err));

		switch (-err) {
		case EBUSY:
			fprintf(stderr, "Hint: XDP already loaded on device"
				" use --force to swap/replace\n");
			break;
		case EOPNOTSUPP:
			fprintf(stderr, "Hint: Native-XDP not supported"
				" use --skb-mode or --auto-mode\n");
			break;
		default:
			break;
		}
		return EXIT_FAIL_XDP;
	}

	return EXIT_OK;
}

static void print_avail_progs(struct bpf_object *obj)
{
	struct bpf_program *pos;

	bpf_object__for_each_program(pos, obj) {
		if (bpf_program__is_xdp(pos))
			printf(" %s\n", bpf_program__title(pos, false));
	}
}

int main(int argc, char **argv)
{
	struct bpf_prog_info info = {};
	__u32 info_len = sizeof(info);

	struct bpf_program *bpf_prog;
	struct bpf_object *bpf_obj;
	int prog_fd = -1;
	int err;

	struct config cfg = {
		.xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_DRV_MODE,
		.ifindex   = -1,
		.do_unload = false,
	};
	/* Set default BPF-ELF object file and BPF program name */
	strncpy(cfg.filename, default_filename, sizeof(cfg.filename));
	strncpy(cfg.progname, default_progname, sizeof(cfg.progname));
	/* Cmdline options can change these */
	parse_cmdline_args(argc, argv, long_options, &cfg, __doc__);

	printf("filename:%s size:%ld progname:%s\n",
	       cfg.filename, sizeof(cfg.filename), cfg.progname);
	/* Required option */
	if (cfg.ifindex == -1) {
		fprintf(stderr, "ERR: required option --dev missing\n");
		usage(argv[0], __doc__, long_options);
		return EXIT_FAIL_OPTION;
	}
	if (cfg.do_unload)
		return xdp_unload(cfg.ifindex, cfg.xdp_flags);

	/* Load the BPF-ELF object file and get back libbpf bpf_object */
	bpf_obj = load_bpf_object_file(cfg.filename);
	if (!bpf_obj) {
		fprintf(stderr, "ERR: loading file: %s\n", cfg.filename);
		return EXIT_FAIL_BPF;
	}

	printf("XXX: bpf_object__name:%s\n", bpf_object__name(bpf_obj));

	print_avail_progs(bpf_obj);

	/* Find a matching BPF progname */
	bpf_prog = bpf_object__find_program_by_title(bpf_obj, cfg.progname);
	if (!bpf_prog) {
		fprintf(stderr, "ERR: finding progname: %s\n", cfg.progname);
		return EXIT_FAIL_BPF;
	}

	prog_fd = bpf_program__fd(bpf_prog);
	if (prog_fd <= 0) {
		fprintf(stderr, "ERR: bpf_program__fd failed\n");
		return EXIT_FAIL_BPF;
	}

	/* At this point: BPF-prog is (only) loaded by the kernel, and prog_fd
	 * is our file-descriptor handle. Next step is attaching this FD to a
	 * kernel hook point, in this case XDP net_device link-level hook.
	 */
	err = xdp_load(cfg, prog_fd);
	if (err)
		return err;

        /* This step is not really needed , BPF-info via bpf-syscall */
	err = bpf_obj_get_info_by_fd(prog_fd, &info, &info_len);
	if (err) {
		fprintf(stderr, "ERR: can't get prog info - %s\n",
			strerror(errno));
		return err;
	}

	printf("Success: Loading "
	       "XDP prog name:%s(id:%d) on device:%s(ifindex:%d)\n",
	       info.name, info.id, cfg.ifname, cfg.ifindex);
	return EXIT_OK;
}
