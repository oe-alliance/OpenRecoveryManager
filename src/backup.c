#define _GNU_SOURCE

#include "backup.h"

#include "boxinfo.h"
#include "files.h"
#include "flash.h"
#include "i18n.h"
#include "process.h"
#include "viewer.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define TITLE _("Back up image")
#define SCRIPT "/tmp/orm-backup.sh"
#define MB (1024LL * 1024LL)
#define MAX_MEDIA 16

/* What runImageBackup() of ImageBackup.py takes from BoxInfo, for the running image only. */
struct plan {
	struct flash_slot slot;
	char box[64];
	char model[64];
	char imagedir[64];
	char kernelfile[64];
	char rootfile[64];
	char mkubifs[160];
	char ubinize[160];
	char mtdkernel[32];
	char distro[64];
	char displaydistro[64];
	char imageversion[64];
	char zip[256];
	int kexec;
	int gpt;
	int small;
	enum { FS_TAR, FS_UBI, FS_JFFS2 } fs;
};

struct medium {
	char path[256];
	long long free;
};

static int is(const char *value, const char *const list[])
{
	for (; *list; ++list)
		if (!strcmp(value, *list))
			return 1;
	return 0;
}

static void wait_key(struct input_context *input, const volatile sig_atomic_t *stop)
{
	enum input_key key;
	do {
		key = input_next(input, 1000);
	} while (key != INPUT_OK && key != INPUT_BACK && !(stop && *stop));
}

static void message(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const char *text)
{
	ui_error(ui, TITLE, text);
	wait_key(input, stop);
}

/* "/dev/mmcblk0p3".split("/")[2] of ImageBackup.py. */
static const char *third_part(const char *path)
{
	const char *p = path;
	int i = 0;
	while (i < 2 && p) {
		p = strchr(p, '/');
		if (p)
			p++;
		++i;
	}
	return p ? p : path;
}

/* A word of imagefs, like "ubi" in imageFs of ImageBackup.py. */
static int has_word(const char *text, const char *word)
{
	size_t length = strlen(word);
	const char *p = text;
	while ((p = strstr(p, word))) {
		if ((p == text || p[-1] == ' ') && (p[length] == ' ' || !p[length]))
			return 1;
		p += length;
	}
	return 0;
}

static void safe_name(char *text)  /* Like safeDistro of ImageBackup.py. */
{
	for (; *text; ++text)
		if (!((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') || (*text >= '0' && *text <= '9') ||
			strchr("._+-", *text)))
			*text = '_';
}

/* The file system of the backup like imageFs of ImageBackup.py, a small flash always gets a tar. */
static int image_fs(const char *imagefs, int small)
{
	if (small)
		return FS_TAR;
	if (has_word(imagefs, "jffs2"))
		return FS_JFFS2;
	if (has_word(imagefs, "ubi"))
		return FS_UBI;
	return FS_TAR;
}

static void make_plan(struct plan *p)
{
	char imagefs[64];
	char cmdline[2048] = "";
	char small[16];
	FILE *file;
	time_t now = time(NULL);
	struct tm local;
	char date[32];
	memset(p, 0, sizeof(*p));
	flash_running_slot(&p->slot);
	boxinfo_box_name(p->box, sizeof(p->box));
	boxinfo_value("model", p->model, sizeof(p->model));
	boxinfo_value("imagedir", p->imagedir, sizeof(p->imagedir));
	boxinfo_value("kernelfile", p->kernelfile, sizeof(p->kernelfile));
	boxinfo_value("rootfile", p->rootfile, sizeof(p->rootfile));
	boxinfo_value("mkubifs", p->mkubifs, sizeof(p->mkubifs));
	boxinfo_value("ubinize", p->ubinize, sizeof(p->ubinize));
	boxinfo_value("mtdkernel", p->mtdkernel, sizeof(p->mtdkernel));
	boxinfo_value("distro", p->distro, sizeof(p->distro));
	boxinfo_value("displaydistro", p->displaydistro, sizeof(p->displaydistro));
	boxinfo_value("imageversion", p->imageversion, sizeof(p->imageversion));
	boxinfo_value("imagefs", imagefs, sizeof(imagefs));
	boxinfo_value("smallflash", small, sizeof(small));
	if ((file = fopen("/proc/cmdline", "r"))) {
		if (!fgets(cmdline, sizeof(cmdline), file))
			cmdline[0] = '\0';
		fclose(file);
	}
	p->kexec = strstr(cmdline, "kexec=1") != NULL;
	p->gpt = (!strcmp(p->model, "dreamone") || !strcmp(p->model, "dreamtwo")) && access("/dev/mmcblk0p7", F_OK) == 0;
	p->small = !strcasecmp(small, "true");
	p->fs = image_fs(imagefs, p->small);
	if (!p->distro[0])
		snprintf(p->distro, sizeof(p->distro), "Unknown");
	if (!p->imageversion[0])
		snprintf(p->imageversion, sizeof(p->imageversion), "Unknown");
	safe_name(p->distro);
	safe_name(p->imageversion);
	localtime_r(&now, &local);
	strftime(date, sizeof(date), "%Y%m%d_%H%M", &local);
	snprintf(p->zip, sizeof(p->zip), "%s-%s-%s-backup-%s_usb.zip", p->distro, p->imageversion, p->box, date);
	for (char *c = p->zip; *c; ++c)  /* Lowercase like the images of the feeds. */
		*c = (char)tolower((unsigned char)*c);
}

/* The size of the running root file system in bytes, without other mounts. */
static long long root_size(void)
{
	char output[128] = "";
	char *const du[] = {"du", "-skx", "/", NULL};
	if (process_capture(du, output, sizeof(output)) != 0)
		return 0;
	return atoll(output) * 1024;
}

/* Media for the backup like keyStart() of ImageBackup.py, only mounted ones: on another device than the folder
 * above, so neither a folder of /media on the flash nor an empty mount point in the tmpfs of /media. */
static int find_media(struct medium *media)
{
	static const char *const bases[] = {"/media", "/media/net"};
	int count = 0;
	for (size_t b = 0; b < 2; ++b) {
		struct dirent *entry;
		struct stat root;
		DIR *d;
		if (stat(bases[b], &root) != 0)
			continue;
		d = opendir(bases[b]);
		if (!d)
			continue;
		while ((entry = readdir(d)) && count < MAX_MEDIA) {
			struct stat info;
			struct statvfs fs;
			char path[256];
			if (entry->d_name[0] == '.' || (!b && !strcmp(entry->d_name, "net")) || !strcmp(entry->d_name, "autofs"))
				continue;
			snprintf(path, sizeof(path), "%s/%.200s", bases[b], entry->d_name);
			if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_dev == root.st_dev ||
				statvfs(path, &fs) != 0 || access(path, W_OK) != 0)
				continue;
			snprintf(media[count].path, sizeof(media[count].path), "%s", path);
			media[count++].free = (long long)fs.f_bavail * (long long)fs.f_frsize;
		}
		closedir(d);
	}
	return count;
}

static void quoted(FILE *f, const char *text)  /* 'text' for the shell. */
{
	fputc('\'', f);
	for (; *text; ++text)
		if (*text == '\'')
			fputs("'\\''", f);
		else
			fputc(*text, f);
	fputc('\'', f);
}

static void say(FILE *f, const char *text)
{
	fputs("echo ", f);
	quoted(f, text);
	fputc('\n', f);
}

static const char *const gbquad4k[] = {"gbquad4k", "gbquad4kpro", "gbue4k", "gbx34k", NULL};
static const char *const hisi_dumps[] = {"h9", "i55plus", NULL};
static const char *const dumps[] = {"fastboot", "bootargs", "baseparam", "pq_param", "logo"};

/* The kernel partition or file of the running image. */
static const char *kernel_source(const struct plan *p)
{
	const struct flash_slot *s = &p->slot;
	if (!s->multiboot)
		return p->mtdkernel;
	if (p->kexec && !strcmp(s->code, "R"))
		return s->kernel;
	return third_part(s->kernel);
}

static void write_start(FILE *f, const char *build, const char *work, const char *mount_point)
{
	fprintf(f, "#!/bin/sh\nstart=$(date +%%s)\n");
	fprintf(f, "cleanup() {\n\trm -rf '%s' '%s'\n\tumount '%s' 2>/dev/null\n\trmdir '%s' /tmp/ib 2>/dev/null\n"
		"\techo 3 > /proc/sys/vm/drop_caches\n\tsync\n}\n", build, work, mount_point, mount_point);
	fprintf(f, "fail() {\n\techo \"$1\"\n\tcleanup\n\texit 1\n}\n");
	fputs("trap 'echo; fail \"The backup was cancelled.\"' TERM\n", f);  /* RED ends the group, the script cleans up. */
	say(f, "The backup can take up to about 15 minutes.");
	fprintf(f, "rm -rf '%s'\nmkdir -p '%s' '%s' || fail ", work, work, mount_point);
	quoted(f, "The working folder cannot be created.");
	fputc('\n', f);
}

static void write_mount(FILE *f, const struct flash_slot *s, const char *rootfs, const char *mount_point)
{
	say(f, "Mounting the root file system.");
	if (!s->multiboot)
		fprintf(f, "mount --bind / '%s'", mount_point);
	else if (!strncmp(rootfs, "ubi0:", 5) || (s->rootsubdir[0] && s->ubi))
		fprintf(f, "mount -t ubifs '%s' '%s'", rootfs, mount_point);
	else
		fprintf(f, "mount '/dev/%s' '%s'", rootfs, mount_point);
	fputs(" || fail ", f);
	quoted(f, "The root file system cannot be mounted.");
	fputc('\n', f);
}

/* The "imageversion" inventory file. */
static void write_image_version(FILE *f, const struct plan *p)
{
	fprintf(f, "{\n\techo '[Image Version]'\n\techo 'distro=%s'\n\techo ", p->distro);
	quoted(f, "displaydistro=");
	fprintf(f, "'%s'\n\techo 'imageversion=%s.'\n\techo\n\techo '[Enigma2 Settings]'\n"
		"\tcat /etc/enigma2/settings\n\techo\n\techo '[Installed Plugins]'\n"
		"\topkg list-installed | grep 'enigma2-plugin-*'\n} > /tmp/imageversion 2>/dev/null\n",
		p->displaydistro, p->imageversion);
	fprintf(f, "echo 3 > /proc/sys/vm/drop_caches\n");
}

/* dreamKernelBackupCommands(): the kernel of the shared bank A, appended to the tar. */
static void write_dream_kernel(FILE *f, const struct plan *p, const char *work, const char *root)
{
	fprintf(f, "stage='%s/dream-kernel'\n", work);
	fprintf(f, "/usr/bin/ofgwrite_bin --backup-dream-kernel-a \"$stage\" || fail 'Dream: kernel A'\n");
	fprintf(f, "mv \"$stage/kernel.bin\" '%s/%s' || fail 'Dream: kernel A'\n", work, p->kernelfile);
	fprintf(f, "if [ -d '%s/var/lib/dpkg/info' ]; then package=dpkg; elif [ -d '%s/var/lib/opkg/info' ]; then "
		"package=opkg; else fail 'Dream: missing package info directory'; fi\n", root, root);
	fprintf(f, "mkdir -p \"$stage/rootfs/var/lib/$package/info\" || exit 1\n");
	fprintf(f, "cp -p \"$stage/kernel-image.postinst\" \"$stage/rootfs/var/lib/$package/info/kernel-image.postinst\" "
		"|| fail 'Dream: postinst'\n");
	fprintf(f, "kernelname=$(readlink \"$stage/rootfs/boot/vmlinux.bin\") || fail 'Dream: vmlinux.bin'\n");
}

static void write_tar(FILE *f, const char *work, const char *root, int dream)
{
	say(f, "Packing the root file system.");
	fprintf(f, "tar -cf '%s/rootfs.tar' -C '%s'", work, root);
	if (dream)
		fputs(" --exclude './boot/vmlinux.bin*' --exclude './boot/vmlinux.gz*' --exclude "
			"./usr/share/fastboot/lcd_anim.bin --exclude ./var/lib/dpkg/info/kernel-image.postinst --exclude "
			"./var/lib/opkg/info/kernel-image.postinst", f);
	fputs(" --exclude ./boot/kernel.img --exclude ./var/nmbd --exclude ./.resizerootfs --exclude ./.resize-rootfs "
		"--exclude ./.resize-linuxrootfs --exclude ./.resize-userdata --exclude ./var/lib/samba/private/msg.sock "
		"--exclude './var/lib/samba/msg.sock/*' --exclude ./run/avahi-daemon/socket --exclude "
		"./run/chrony/chronyd.sock --exclude ./run/udev/control . || fail ", f);
	quoted(f, "The root file system cannot be packed.");
	fputc('\n', f);
	if (dream)
		fprintf(f, "tar -rf '%s/rootfs.tar' -C \"$stage/rootfs\" ./boot/vmlinux.bin \"./boot/$kernelname\" "
			"./usr/share/fastboot/lcd_anim.bin \"./var/lib/$package/info/kernel-image.postinst\" || "
			"fail 'Dream: kernel files'\n", work);
	fprintf(f, "sync\n");
	say(f, "Compressing the root file system, this takes the most time.");
	fprintf(f, "bzip2 '%s/rootfs.tar' || fail ", work);
}

static void write_root_fs(FILE *f, const struct plan *p, const char *work, const char *root, int dream)
{
	if (p->fs == FS_JFFS2) {
		say(f, "Creating the journaling flash file system.");
		fprintf(f, "mkfs.jffs2 --root='%s' --faketime --output='%s/root.jffs2' %s || fail ", root, work, p->mkubifs);
	} else if (p->fs == FS_UBI) {
		say(f, "Creating the UBI file system.");
		fprintf(f, "echo > '%s/root.ubi'\nmkfs.ubifs -r '%s' -o '%s/root.ubi' %s || fail ", work, root, work,
			p->mkubifs);
		quoted(f, "The root file system cannot be packed.");
		fprintf(f, "\nprintf '[ubifs]\\nmode=ubi\\nimage=%s/root.ubi\\nvol_id=0\\nvol_type=dynamic\\n"
			"vol_name=rootfs\\nvol_flags=autoresize\\n' > '%s/ubinize.cfg'\n", work, work);
		fprintf(f, "ubinize -o '%s/root.ubifs' %s '%s/ubinize.cfg' || fail ", work, p->ubinize, work);
	} else
		write_tar(f, work, root, dream);
	quoted(f, "The root file system cannot be packed.");
	fprintf(f, "\nsync\n");
}

static void write_kernel(FILE *f, const struct plan *p, const char *work, int by_partition, int dream)
{
	say(f, "Saving the kernel.");
	if (dream)
		fprintf(f, "test -s '%s/%s' || fail 'Dream: kernel A'\n", work, p->kernelfile);
	else if (by_partition && (p->kexec || p->gpt))
		fprintf(f, "cp '/%s' '%s/%s' || fail ", kernel_source(p), work, p->kernelfile);
	else if (by_partition)
		fprintf(f, "dd if='/dev/%s' of='%s/%s' 2>&1 || fail ", kernel_source(p), work, p->kernelfile);
	else
		fprintf(f, "nanddump -a -f '%s/vmlinux.gz' '/dev/%s' || fail ", work, p->mtdkernel);
	if (!dream) {
		quoted(f, "The kernel cannot be saved.");
		fputc('\n', f);
	}
}

/* The file that tells the flashing how to treat the backup. */
static void write_force(FILE *f, const struct plan *p, const char *dest)
{
	static const char *const reboot_update[] = {"vuultimo4k", "vusolo4k", "vuduo2", "vusolo2", "vusolo", "vuduo",
		"vuultimo", "vuuno", NULL};
	static const char *const force_update[] = {"vuzero", "vusolose", "vuuno4k", "vuzero4k", NULL};
	static const char *const force[] = {"viperslim", "evoslimse", "evoslimt2c", "novaip", "zgemmai55", "sf98",
		"xpeedlxpro", "evoslim", "vipert2c", NULL};
	if (is(p->box, reboot_update))
		fprintf(f, "echo 'This file forces a reboot after the update.' > '%s/reboot.update'\n", dest);
	else if (is(p->box, force_update))
		fprintf(f, "echo 'This file forces the update.' > '%s/force.update'\n", dest);
	else if (is(p->box, force))
		fprintf(f, "echo 'This file forces the update.' > '%s/force'\n", dest);
	else if (p->slot.multiboot && p->slot.rootsubdir[0])
		fprintf(f, "printf \"Rename the 'unforce_%s.txt' to 'force_%s.txt' and move it to the root of your usb-stick.\\n"
			"When you enter the recovery menu then it will force to install the image in the linux1 selection.\\n\" > "
			"'%s/force_%s_READ.ME'\n", p->model, p->model, dest, p->model);
	else
		fprintf(f, "echo \"Rename this file to 'force' to force an update without confirmation.\" > '%s/noforce'\n",
			dest);
}

/* The boot loader and display files some receivers need next to the image. */
static void write_box_files(FILE *f, const struct plan *p, const char *work, const char *build, const char *dest)
{
	static const char *const lcd_gb[] = {"gbquad", "gbquadplus", "gb800ue", "gb800ueplus", "gbultraue", "gbultraueh",
		"twinboxlcd", "twinboxlcdci", "singleboxlcd", "sf208", "sf228", NULL};
	static const char *const lcd_flashing[] = {"e4hdultra", "protek4k", NULL};
	if (is(p->box, gbquad4k)) {
		fprintf(f, "mv '%s/rescue.bin' '%s/'\n", work, dest);
		fprintf(f, "for file in boot.bin gpt.bin boot4.bin gpt4.bin; do\n\t[ -f \"/usr/share/$file\" ] && "
			"cp -f \"/usr/share/$file\" '%s/'\ndone\n", dest);
	}
	if (is(p->model, hisi_dumps)) {
		for (size_t i = 0; i < 5; ++i)
			fprintf(f, "mv '%s/%s.bin' '%s/'\n", work, dumps[i], dest);
		fprintf(f, "cp -f /usr/share/fastboot.bin /usr/share/bootargs.bin '%s/'\n", build);
	}
	if (is(p->box, lcd_gb))
		fprintf(f, "for file in lcdwaitkey.bin lcdwarning.bin; do\n\t[ -f \"/usr/share/$file\" ] && "
			"cp \"/usr/share/$file\" '%s/'\ndone\n", dest);
	if (is(p->box, lcd_flashing))
		fprintf(f, "[ -f /usr/share/lcdflashing.bmp ] && cp /usr/share/lcdflashing.bmp '%s/'\n", dest);
	if (!strcmp(p->box, "gb800solo"))
		fprintf(f, "printf 'flash -noheader usbdisk0:gigablue/solo/kernel.bin flash0.kernel\\n"
			"flash -noheader usbdisk0:gigablue/solo/rootfs.bin flash0.rootfs\\n"
			"setenv -p STARTUP \"boot -z -elf flash0.kernel: '\\''rootfstype=jffs2 bmem=106M@150M root=/dev/mtdblock6 "
			"rw '\\''\"\\n' > '%s/burn.bat'\n", build);
}

/* The shell script of runImageBackup() for the running image, without the USB recovery images. Its output is a
 * log and stays English like the one of opkg or ofgwrite. */
static int write_script(const struct plan *p, const char *media)
{
	static const char *const dream_kernel_a[] = {"dm820", "dm7080", NULL};
	static const char *const kernel_dd[] = {"h8", "h8se", "hzero", NULL};
	static const char *const empty_kernel[] = {"dm800se", "dm500hd", "dreamone", "dreamtwo", NULL};
	const struct flash_slot *s = &p->slot;
	int dream = is(p->box, dream_kernel_a);
	int by_partition = s->multiboot || !strncmp(p->mtdkernel, "mmcblk0", 7) || is(p->model, kernel_dd);
	const char *rootfs = s->ubi ? s->device : third_part(s->device);
	const char *rootfile = p->small ? "rootfs.tar.bz2" : p->rootfile;
	char target[320];
	char work[340];
	char mount_point[64];
	char root[128];
	char build[400];
	char dest[480];
	FILE *f = fopen(SCRIPT, "w");
	if (!f)
		return 0;
	snprintf(target, sizeof(target), "%s/images", media);
	snprintf(work, sizeof(work), "%s/ib", target);
	snprintf(mount_point, sizeof(mount_point), "%s", s->rootsubdir[0] ? "/tmp/ib/RootSubdir" : "/tmp/ib/root");
	if (s->multiboot && s->rootsubdir[0])
		snprintf(root, sizeof(root), "%s/%s", mount_point, s->rootsubdir);
	else
		snprintf(root, sizeof(root), "%s", mount_point);
	snprintf(build, sizeof(build), "%s/build_%s", target, p->box);
	snprintf(dest, sizeof(dest), "%s/%s", build, p->imagedir);
	write_start(f, build, work, mount_point);
	write_mount(f, s, rootfs, mount_point);
	write_image_version(f, p);
	if (dream)
		write_dream_kernel(f, p, work, root);
	write_root_fs(f, p, work, root, dream);
	if (is(p->box, gbquad4k))
		fprintf(f, "dd if=/dev/mmcblk0p3 of='%s/rescue.bin' 2>&1\n", work);
	if (is(p->model, hisi_dumps))
		for (size_t i = 0; i < 5; ++i)
			fprintf(f, "dd if=/dev/mtd%d of='%s/%s.bin' 2>&1\n", (int)i, work, dumps[i]);
	write_kernel(f, p, work, by_partition, dream);
	/* The folder the zip holds, the same as the FlashManager and ofgwrite expect. */
	say(f, "Putting the backup together.");
	fprintf(f, "rm -rf '%s'\nmkdir -p '%s'\nmv /tmp/imageversion '%s/'\n", build, dest, dest);
	fprintf(f, "for file in usr/lib/enigma.info usr/lib/enigma.conf etc/image-version; do\n"
		"\t[ -f \"%s/$file\" ] && cp \"%s/$file\" '%s/'\ndone\n", root, root, dest);
	if (is(p->model, empty_kernel))
		fprintf(f, "touch '%s/%s'\n", dest, p->kernelfile);
	else if (by_partition || dream)
		fprintf(f, "mv '%s/%s' '%s/'\n", work, p->kernelfile, dest);
	else
		fprintf(f, "mv '%s/vmlinux.gz' '%s/%s'\n", work, dest, p->kernelfile);
	if (p->fs == FS_TAR)  /* bzip2 always writes rootfs.tar.bz2, rootfile may be rootfs-one.tar.bz2. */
		fprintf(f, "mv '%s/rootfs.tar.bz2' '%s/%s'\n", work, dest, rootfile);
	else
		fprintf(f, "mv '%s/%s' '%s/%s'\n", work, p->fs == FS_UBI ? "root.ubifs" : "root.jffs2", dest, rootfile);
	write_force(f, p, dest);
	write_box_files(f, p, work, build, dest);
	fprintf(f, "echo 3 > /proc/sys/vm/drop_caches\n");
	say(f, "Creating the zip file.");
	fprintf(f, "7za a -r -bt -bd -bso0 '%s/%s' '%s'/* || fail ", target, p->zip, build);
	quoted(f, "The zip file cannot be created, is there enough free space?");
	fputc('\n', f);
	fprintf(f, "[ -r '%s/%s' -a -r '%s/%s' ] || fail ", dest, p->kernelfile, dest, rootfile);
	quoted(f, "The backup is incomplete.");
	fprintf(f, "\ncleanup\nseconds=$(( $(date +%%s) - start ))\necho\n"
		"echo \"Duration: $(( seconds / 60 ))m$(printf %%02d $(( seconds %% 60 )))s\"\n");
	return fclose(f) == 0;
}

/* The rows and marks of the media, grey for too small; the first one with enough space, else -1. */
static int media_rows(struct medium *media, int count, long long need, char (*rows)[320], const char **items, char *marks)
{
	int fits = -1;
	for (int i = 0; i < count; ++i) {
		struct statvfs fs;
		if (statvfs(media[i].path, &fs) == 0)
			media[i].free = (long long)fs.f_bavail * (long long)fs.f_frsize;
		snprintf(rows[i], sizeof(rows[i]), _("%s\t%lld MB"), media[i].path, media[i].free / MB);
		items[i] = rows[i];
		marks[i] = media[i].free < need ? 2 : 0;  /* Grey: too small. */
		if (fits < 0 && !marks[i])
			fits = i;
	}
	return fits;
}

/* Grey media can be chosen too, YELLOW frees up space on them. */
static int choose_medium(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	struct medium *media, int count, long long need)
{
	char rows[MAX_MEDIA][320];
	const char *items[MAX_MEDIA];
	char marks[MAX_MEDIA] = {0};
	char body[512];
	char header[96];
	char footer[160];
	int selected = media_rows(media, count, need, rows, items, marks);
	if (selected < 0)
		selected = 0;
	snprintf(header, sizeof(header), "%s\t%s", _("Medium"), _("Free"));
	while (!(stop && *stop)) {
		enum input_key key;
		if (media_rows(media, count, need, rows, items, marks) < 0)
			snprintf(body, sizeof(body), _("There is not enough free space on any medium. The backup needs about "
				"%lld MB. YELLOW frees up space on the chosen medium."), need / MB);
		else
			snprintf(body, sizeof(body), _("Saves the running image as a zip file, which Flash online/local can flash "
				"again. It needs about %lld MB. Where should it be saved?"), need / MB);
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"),
			.ok = marks[selected] ? NULL : _("Back up"), .yellow = _("Free up space"), .back = _("Menu")});
		ui_menu_table(ui, &(struct ui_menu){.title = TITLE, .body = body, .header = header, .items = items,
			.count = count, .selected = selected, .marks = marks, .marked = -1, .align = "lr", .footer = footer});
		key = input_next(input, 1000);
		selected = list_move(key, selected, count);
		if (key == INPUT_OK && !marks[selected])
			return selected;
		if (key == INPUT_YELLOW)
			files_free_space(ui, input, stop, media[selected].path, need);
		else if (key == INPUT_BACK)
			return -1;
	}
	return -1;
}

struct running {
	struct live_output output;
	struct input_context *input;
	const volatile sig_atomic_t *stop;
	int cancelled;
};

static void backup_tick(void *opaque)
{
	struct running *r = opaque;
	live_output_tick(&r->output);
	if (!r->cancelled && input_next(r->input, 0) == INPUT_RED &&
		ask(r->output.ui, r->input, r->stop, TITLE, _("Cancel the backup? What is done so far is deleted."), 0)) {
		r->cancelled = 1;
		process_cancel();
	}
}

static void backup_line(const char *line, void *opaque)
{
	live_output_line(line, &((struct running *)opaque)->output);
}

void image_backup(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop)
{
	struct medium media[MAX_MEDIA];
	struct running run = {.output = {.ui = ui}, .input = input, .stop = stop};
	char footer[160];
	struct plan plan;
	char title[320];
	char note[512];
	long long need;
	int count;
	int choice;
	int result;
	ui_progress(ui, TITLE, _("Measuring the running image."), 20, _("Please wait..."), _("Please wait..."));
	make_plan(&plan);
	/* tar and bzip2 side by side, then the zip next to the compressed root, like the script runs. */
	need = root_size() * 3 / 2 + 50 * MB;
	count = find_media(media);
	if (!count) {
		message(ui, input, stop, _("No medium is mounted, the backup needs a USB stick or a hard disk."));
		return;
	}
	if ((choice = choose_medium(ui, input, stop, media, count, need)) < 0)
		return;
	if (!write_script(&plan, media[choice].path)) {
		message(ui, input, stop, _("The backup cannot be prepared."));
		return;
	}
	snprintf(title, sizeof(title), "%s - %s", TITLE, media[choice].path);
	snprintf(footer, sizeof(footer), "%s   RED: %s", _("Please wait, do not switch off the receiver..."), _("Cancel"));
	run.output.title = title;
	run.output.footer = footer;
	ui_busy(ui, 1);  /* The footer offers RED. */
	{
		char *const sh[] = {"/bin/sh", SCRIPT, NULL};
		result = process_run_with_updates(sh, NULL, backup_line, backup_tick, 100, &run);
	}
	ui_busy(ui, 0);
	unlink(SCRIPT);
	if (result == 0)
		snprintf(note, sizeof(note), _("The backup is saved as %s/images/%s."), media[choice].path, plan.zip);
	else
		snprintf(note, sizeof(note), "%s", run.cancelled ? _("The backup was cancelled.") : _("The backup failed."));
	live_output_view(&run.output, input, stop, note, result == 0);
	live_output_free(&run.output);
}
