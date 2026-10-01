#define _GNU_SOURCE

#include "flash.h"

#include "boxinfo.h"
#include "i18n.h"
#include "process.h"
#include "reset.h"
#include "viewer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define TITLE _("Flash online/local")
#define OFGWRITE "/usr/bin/ofgwrite"
#define FEED_FILE "/tmp/orm-feed.json"
#define FLASH_LOG "/tmp/ofgwrite.log"
#define OFGWRITE_WIDTH 640  /* The window of ofgwrite, in the middle of the screen. */
#define OFGWRITE_HEIGHT 480
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"
#define OPENATV_FEED "https://images.mynonpublic.com/openatv/json/"
#define DISTRIBUTIONS "https://raw.githubusercontent.com/OpenATV/FlashImage/gh-pages/"
#define DUAL_BOOT_FILE "/dev/block/by-name/flag"
#define DREAM_BOOT_FILE "/data/bootconfig.txt"
#define MAX_IMAGES 500
#define MAX_FEEDS 16
#define MAX_SLOTS 24
#define MAX_ARGS 12
#define MB (1024LL * 1024LL)

struct image {
	char category[96];
	char name[160];
	char link[512];  /* A URL or the path of a local zip. */
	long long size;  /* 0 when unknown. */
	int local;
};

struct feed {
	char name[32];
	char url[256];
};

struct slot {
	char code[8];
	char device[64];  /* /dev/... or ubi0:... */
	char kernel[64];
	char rootsubdir[32];
	char line[1024];
	int ubi;
	int uuid;
};

/* The running slot and everything the ofgwrite arguments of the FlashManager depend on. */
struct target {
	int multiboot;
	struct slot slot;
	char model[32];
	char mtdkernel[32];
	char mtdrootfs[32];
	char args[MAX_ARGS][80];
	int arg_count;
};

static void wait_key(struct input_context *input, const volatile sig_atomic_t *stop)
{
	enum input_key key;
	do {
		key = input_next(input, 1000);
	} while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED && !(stop && *stop));
}

static void message(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const char *text)
{
	ui_error(ui, TITLE, text);
	wait_key(input, stop);
}

static int true_value(const char *key)
{
	char value[16];
	boxinfo_value(key, value, sizeof(value));
	return !strcasecmp(value, "true");
}

static void read_line(const char *path, char *line, size_t size)  /* All lines joined by a space. */
{
	char buffer[1024];
	FILE *file = fopen(path, "r");
	line[0] = '\0';
	if (!file)
		return;
	while (fgets(buffer, sizeof(buffer), file)) {
		char *start = buffer;
		char *end;
		while (*start == ' ' || *start == '\t')
			start++;
		end = start + strlen(start);
		while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		if (!*start)
			continue;
		if (line[0])
			strncat(line, " ", size - strlen(line) - 1);
		strncat(line, start, size - strlen(line) - 1);
	}
	fclose(file);
}

/* getParam() of MultiBoot.py: the last "name=" in the line, also behind "bootargs=" or a quote. */
static int param(const char *line, const char *name, char *value, size_t size)
{
	char key[32];
	const char *found = NULL;
	const char *at = line;
	size_t length;
	snprintf(key, sizeof(key), "%s=", name);
	while ((at = strstr(at, key))) {
		if (at == line || strchr(" ='\"", at[-1]))
			found = at;
		at++;
	}
	if (!found)
		return 0;
	found += strlen(key);
	length = strcspn(found, " '\"");
	snprintf(value, size, "%.*s", (int)length, found);
	return 1;
}

static void uuid_device(char *device, size_t size)  /* getUUIDtoDevice() of MultiBoot.py */
{
	const char *uuid = device + (strncmp(device, "UUID=", 5) ? 0 : 5);
	DIR *dir = opendir("/dev/uuid");
	struct dirent *entry;
	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		char path[300];
		char value[128];
		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/dev/uuid/%.255s", entry->d_name);
		read_line(path, value, sizeof(value));
		if (!strcmp(value, uuid)) {
			snprintf(device, size, "/dev/%.50s", entry->d_name);
			break;
		}
	}
	closedir(dir);
}

static int by_name(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

/* The slot code of a STARTUP_* file, empty for none. */
static void slot_code(const char *file, char *code, size_t size)
{
	const char *underscore = strrchr(file, '_');
	if (!strcmp(file, "STARTUP_ANDROID"))
		snprintf(code, size, "%s", "A");
	else if (!strcmp(file, "STARTUP_ANDROID_LINUXSE"))
		snprintf(code, size, "%s", "L");
	else if (!strcmp(file, "STARTUP_RECOVERY"))
		snprintf(code, size, "%s", "R");
	else if (!strcmp(file, "STARTUP_FLASH"))
		snprintf(code, size, "%s", "F");
	else if (strstr(file, "BOXMODE")) {  /* STARTUP_LINUX_1_BOXMODE_12 */
		const char *end = strstr(file, "_BOXMODE");
		const char *start = end;
		while (start > file && start[-1] != '_')
			start--;
		snprintf(code, size, "%.*s", (int)(end - start), start);
	} else if (underscore)
		snprintf(code, size, "%.7s", underscore + 1);
}

static const char *find_disk(const char *line)
{
	static const char *const disks[] = {"sda", "sdb", "sdc", "sdd"};
	for (int j = 0; j < 4; ++j)
		if (strstr(line, disks[j]))
			return disks[j];
	return NULL;
}

static void slot_kernel(struct slot *s)
{
	const char *disk;
	const char *p;
	if (strstr(s->line, "rootsubdir")) {
		param(s->line, "kernel", s->kernel, sizeof(s->kernel));
		param(s->line, "rootsubdir", s->rootsubdir, sizeof(s->rootsubdir));
		return;
	}
	if (strstr(s->line, "flash=1")) {
		param(s->line, "kernel", s->kernel, sizeof(s->kernel));
		return;
	}
	disk = find_disk(s->line);
	if (disk) {
		const char *rest = strstr(s->line, disk) + 3;
		snprintf(s->kernel, sizeof(s->kernel), "/dev/%s%.*s", disk, (int)strcspn(rest, " "), rest);
		return;
	}
	p = strrchr(s->device, 'p');
	if (p && p[1] >= '0' && p[1] <= '9')
		snprintf(s->kernel, sizeof(s->kernel), "%.*sp%d", (int)(p - s->device), s->device, atoi(p + 1) - 1);
}

/* One STARTUP_* file of dir, 0 for none with a root of Linux. */
static int read_slot(const char *dir, const char *file, struct slot *s)
{
	char path[512];
	memset(s, 0, sizeof(*s));
	if (strncmp(file, "STARTUP_", 8) || strstr(file, "DISABLE"))
		return 0;
	slot_code(file, s->code, sizeof(s->code));
	if (!s->code[0])
		return 0;
	snprintf(path, sizeof(path), "%s/%.255s", dir, file);
	read_line(path, s->line, sizeof(s->line));
	if (!param(s->line, "root", s->device, sizeof(s->device)))
		return 0;  /* Android or recovery without a root of Linux. */
	if (strstr(s->device, "UUID="))
		uuid_device(s->device, sizeof(s->device));
	if (access(s->device, F_OK) != 0 && strcmp(s->device, "ubi0:ubifs") &&
		strcmp(s->device, "ubi0:rootfs") && strcmp(s->device, "ubi0:dreambox-rootfs"))
		return 0;
	s->ubi = strstr(s->line, "ubi.mtd=") != NULL;
	s->uuid = strstr(s->line, "UUID=") != NULL;
	if (strstr(s->line, "rescuemode"))
		strcpy(s->rootsubdir, "rescue");
	slot_kernel(s);
	return 1;
}

static int has_code(const struct slot *slots, int count, const char *code)
{
	for (int j = 0; j < count; ++j)
		if (!strcmp(slots[j].code, code))
			return 1;
	return 0;
}

/* loadBootSlots() of MultiBoot.py for the STARTUP_* files in dir. */
static int read_slots(const char *dir, struct slot *slots)
{
	struct dirent **names;
	int count = 0;
	int total = scandir(dir, &names, NULL, by_name);
	for (int i = 0; i < total; ++i) {
		struct slot s;
		if (read_slot(dir, names[i]->d_name, &s) && !has_code(slots, count, s.code) && count < MAX_SLOTS)
			slots[count++] = s;
	}
	for (int i = 0; i < total; ++i)
		free(names[i]);
	if (total > 0)
		free(names);
	return count;
}

static int run_quiet(char *const argv[])
{
	return process_run(argv, NULL, NULL, NULL);
}

/* The slots of one boot device, mounted for the time being; startup gets its STARTUP line. */
static int device_slots(const char *device, struct slot *slots, char *startup, size_t size)
{
	char dir[] = "/tmp/orm-boot-XXXXXX";
	char path[64];
	char source[64];
	int count = 0;
	if (access(device, F_OK) != 0 || !mkdtemp(dir))
		return 0;
	snprintf(source, sizeof(source), "%s", device);
	char *const mount[] = {"mount", source, dir, NULL};  /* Read-only fails, it is mounted on /boot. */
	if (run_quiet(mount) == 0) {
		char *const umount[] = {"umount", dir, NULL};
		snprintf(path, sizeof(path), "%s/cmdline.txt", dir);
		if (access(path, F_OK) != 0)
			snprintf(path, sizeof(path), "%s/STARTUP", dir);
		if (access(path, F_OK) == 0) {
			read_line(path, startup, size);
			count = read_slots(dir, slots);
		}
		run_quiet(umount);
	}
	rmdir(dir);
	return count;
}

/* The slot the dual boot flag names, count for none. */
static int flag_slot(const struct slot *slots, int count)
{
	char name[8];
	int code;
	int i = 0;
	FILE *flag = fopen(DUAL_BOOT_FILE, "rb");
	if (!flag)
		return count;
	code = fgetc(flag);
	fclose(flag);
	snprintf(name, sizeof(name), "%d", code);
	if (code == EOF)
		return 0;
	while (i < count && strcmp(slots[i].code, name))
		++i;
	return i;
}

/* The running slot like MultiBoot.py, but found by /proc/cmdline first: the image that crashed, not the next one. */
static void find_slot(struct target *t)
{
	static const char *const devices[] = {"/dev/mmcblk0p1", "/dev/mmcblk1p1", "/dev/mmcblk0p3", "/dev/mmcblk0p4",
		"/dev/mtdblock2", "/dev/block/by-name/bootoptions", "/dev/block/by-name/others", "/dev/block/by-name/startup"};
	static const char *const kexec_devices[] = {"/dev/mmcblk0p4", "/dev/mmcblk0p7", "/dev/mmcblk0p9"};
	struct slot *slots = calloc(MAX_SLOTS, sizeof(*slots));
	char cmdline[2048];
	char root[64] = "";
	char subdir[32] = "";
	char startup[1024] = "";
	int kexec;
	int count = 0;
	int i = 0;
	int n;
	const char *const *list;
	if (!slots)
		return;
	read_line("/proc/cmdline", cmdline, sizeof(cmdline));
	kexec = strstr(cmdline, "kexec=1") != NULL;
	list = kexec ? kexec_devices : devices;
	n = kexec ? 3 : 8;
	while (i < n && !count) {
		count = device_slots(list[i], slots, startup, sizeof(startup));
		++i;
	}
	if (count && access(DREAM_BOOT_FILE, F_OK) == 0 && strstr(cmdline, "root=/dev/mmcblk1p") &&
		access("/dev/disk/by-label/DREAMCARD", F_OK) != 0)
		count = 0;  /* canMultiBoot() */
	param(cmdline, "root", root, sizeof(root));
	if (strstr(root, "UUID="))
		uuid_device(root, sizeof(root));
	param(cmdline, "rootsubdir", subdir, sizeof(subdir));
	i = 0;
	while (i < count && (strcmp(slots[i].device, root) || strcmp(slots[i].rootsubdir, subdir)))
		++i;
	if (i == count)
		i = flag_slot(slots, count);
	if (i == count) {
		i = 0;
		while (i < count && strcmp(slots[i].line, startup))
			++i;
	}
	if (i < count) {
		t->multiboot = 1;
		t->slot = slots[i];
	}
	free(slots);
}

void flash_running_slot(struct flash_slot *slot)
{
	struct target t;
	memset(&t, 0, sizeof(t));
	memset(slot, 0, sizeof(*slot));
	find_slot(&t);
	slot->multiboot = t.multiboot;
	slot->ubi = t.slot.ubi;
	snprintf(slot->code, sizeof(slot->code), "%s", t.slot.code);
	snprintf(slot->device, sizeof(slot->device), "%s", t.slot.device);
	snprintf(slot->kernel, sizeof(slot->kernel), "%s", t.slot.kernel);
	snprintf(slot->rootsubdir, sizeof(slot->rootsubdir), "%s", t.slot.rootsubdir);
}

static int is_fat32(const char *device)
{
	char buffer[8];
	int fd = open(device, O_RDONLY);
	int ok = fd >= 0 && pread(fd, buffer, sizeof(buffer), 82) == (ssize_t)sizeof(buffer) && !memcmp(buffer, "FAT32", 5);
	if (fd >= 0)
		close(fd);
	return ok;
}

static void add_arg(struct target *t, const char *option, const char *value)  /* "-r" + "mmcblk0p3" */
{
	if (t->arg_count < MAX_ARGS)
		snprintf(t->args[t->arg_count++], sizeof(t->args[0]), "%s%s", option, value ? value : "");
}

static const char *short_device(const char *device)  /* "/dev/mmcblk0p3".split("/")[2] */
{
	return strncmp(device, "/dev/", 5) ? device : device + 5;
}

static int dream_kernel_a(void)
{
	char output[512] = "";
	char *const features[] = {"/usr/bin/ofgwrite_bin", "--features", NULL};
	return process_capture(features, output, sizeof(output)) == 0 && strstr(output, "dream-kernel-a");
}

static void kexec_args(struct target *t, const char *rootfs)
{
	const char *code = t->slot.code;
	if (!strcmp(code, "R")) {
		add_arg(t, "-r", NULL);
		add_arg(t, "-k", NULL);
		add_arg(t, "-f", NULL);
		return;
	}
	add_arg(t, "-r", rootfs);
	add_arg(t, "-k", NULL);
	if (t->slot.uuid && !strstr(rootfs, "mmcblk")) {
		char slotname[48];
		snprintf(slotname, sizeof(slotname), "%.30s/linuxrootfs", t->model + (strlen(t->model) > 2 ? 2 : 0));
		add_arg(t, "-s", slotname);
	}
	add_arg(t, "-m", code);
}

static void dm7080_args(struct target *t, const char *rootfs, int subdir)  /* dm820 and dm7080 */
{
	const char *code = t->slot.code;
	int kernel_a = dream_kernel_a();
	if (!subdir)
		add_arg(t, kernel_a ? "-r" : "-rmmcblk0p1", NULL);
	else {
		add_arg(t, "-r", rootfs);
		add_arg(t, "-c", code);
		add_arg(t, "-m", code);
	}
	if (kernel_a && (!subdir || (!strcmp(rootfs, "mmcblk0p1") && !strcmp(t->slot.rootsubdir, "linuxrootfs1"))))
		add_arg(t, "-k", NULL);
}

static void multiboot_args(struct target *t, const char *kernel, const char *rootfs, int dream, int subdir)
{
	const char *code = t->slot.code;
	int chkroot = (is_fat32("/dev/block/by-name/others") || access("/dev/block/by-name/startup", F_OK) == 0) && !dream;
	int native = code[0] >= '0' && code[0] <= '9' && !chkroot && !true_value("hasUBIMB") &&
		strstr(t->slot.line, "extra=true");  /* isNewNativeAdditionalSlot() */
	if (true_value("chkrootmb") || native) {  /* Slots sharing a kernel; the flash slot F flashes it below. */
		add_arg(t, "-r", rootfs);
		add_arg(t, "-c", code);
		add_arg(t, "-m", code);
	} else if (!subdir) {
		add_arg(t, "-r", rootfs);
		add_arg(t, "-k", kernel);
		add_arg(t, "-m0", NULL);
	} else {
		add_arg(t, "-r", NULL);
		add_arg(t, "-k", NULL);
		add_arg(t, "-m", code);
	}
}

static void single_args(struct target *t, const char *kernel, const char *rootfs)
{
	if (!strcmp(t->model, "dm800se") || !strcmp(t->model, "dm500hd")) {
		add_arg(t, "-r", rootfs);
		add_arg(t, "-f", NULL);
	} else if (!strcmp(t->model, "zgemmah82h")) {
		add_arg(t, "-r", NULL);
		add_arg(t, "-k", NULL);
		add_arg(t, "-f", NULL);
	} else if (!strcmp(kernel, rootfs))
		add_arg(t, "-r", NULL);
	else {
		add_arg(t, "-r", NULL);
		add_arg(t, "-k", NULL);
	}
}

/* The arguments of flashImage() in the FlashManager for the running slot. */
static void ofgwrite_args(struct target *t)
{
	char cmdline[2048];
	const char *kernel = t->mtdkernel;
	const char *rootfs = t->mtdrootfs;
	const char *code = t->slot.code;
	int dream = !strcmp(t->model, "dreamone") || !strcmp(t->model, "dreamtwo");
	int gpt = dream && access("/dev/mmcblk0p7", F_OK) == 0;
	int kexec;
	int subdir = t->multiboot && t->slot.rootsubdir[0];
	char root[64];
	read_line("/proc/cmdline", cmdline, sizeof(cmdline));
	kexec = strstr(cmdline, "kexec=1") != NULL;
	t->arg_count = 0;
	if (t->multiboot) {
		kernel = kexec ? t->slot.kernel : short_device(t->slot.kernel);
		rootfs = t->slot.ubi ? t->slot.device : short_device(t->slot.device);
	} else if (dream && param(cmdline, "root", root, sizeof(root)) && !strncmp(root, "/dev/mmcblk1p", 13))
		rootfs = root + 5;
	if (kexec && t->multiboot)
		kexec_args(t, rootfs);
	else if (dream && !gpt) {
		add_arg(t, "-r", rootfs);
		add_arg(t, "-k", kernel);
	} else if (dream) {
		add_arg(t, "-r", rootfs);
		add_arg(t, "-a", NULL);
	} else if (!strcmp(t->model, "dm820") || !strcmp(t->model, "dm7080"))
		dm7080_args(t, rootfs, subdir);
	else if (t->multiboot && strcmp(code, "R") && strcmp(code, "F"))
		multiboot_args(t, kernel, rootfs, dream, subdir);
	else
		single_args(t, kernel, rootfs);
}

/* A mounted /media/hdd, else /media/usb, like the FlashManager. */
static int find_media(char *media, size_t size)
{
	static const char *const paths[] = {"/media/hdd", "/media/usb"};
	char line[512];
	for (int i = 0; i < 2; ++i) {
		FILE *mounts = fopen("/proc/mounts", "r");
		int found = 0;
		if (!mounts)
			return 0;
		while (!found && fgets(line, sizeof(line), mounts)) {
			char device[256];
			char point[256];
			if (sscanf(line, "%255s %255s", device, point) == 2 && !strcmp(point, paths[i]))
				found = 1;
		}
		fclose(mounts);
		if (found && access(paths[i], W_OK) == 0) {
			snprintf(media, size, "%s", paths[i]);
			return 1;
		}
	}
	return 0;
}

static int remove_entry(const char *path, const struct stat *info, int flag, struct FTW *ftw)
{
	(void)info;
	(void)flag;
	(void)ftw;
	return remove(path);
}

static void remove_tree(const char *path)
{
	nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static int image_part(const char *name)
{
	static const char *const files[] = {"zImage", "uImage", "root_cfe_auto.bin", "root_cfe_auto.jffs2",
		"oe_kernel.bin", "oe_rootfs.bin", "e2jffs2.img", "rootfs.ubi", "rootfs.bin", "rootfs.tar.bz2",
		"rootfs-one.tar.bz2", "rootfs-two.tar.bz2"};
	for (size_t j = 0; j < sizeof(files) / sizeof(files[0]); ++j)
		if (!strcmp(name, files[j]))
			return 1;
	return 0;
}

/* checkImageFiles() of the FlashManager, for the names without folders. */
static int image_files(char *const names[], int count)
{
	int packed = 0;
	int parts = 0;
	for (int i = 0; i < count; ++i) {
		const char *name = names[i];
		size_t length = strlen(name);
		if ((length > 4 && !strcmp(name + length - 4, ".nfi")) || (length > 7 && !strcmp(name + length - 7, ".tar.xz")))
			packed++;
		if ((strstr(name, "kernel") && length > 4 && !strcmp(name + length - 4, ".bin")) || image_part(name))
			parts++;
	}
	return packed == 1 || parts >= 2;
}

struct zip_names {
	char *names[64];
	int count;
};

static void zip_line(const char *line, void *opaque)  /* unzip -l: "  Length  Date  Time  Name" */
{
	struct zip_names *z = opaque;
	const char *name = strrchr(line, ' ');
	const char *slash;
	if (!name || z->count >= 64 || strchr(line, ':') == NULL)
		return;
	name++;
	slash = strrchr(name, '/');
	if (slash)
		name = slash + 1;
	if (*name)
		z->names[z->count++] = strdup(name);
}

static int valid_zip(const char *path)
{
	struct zip_names z = {.count = 0};
	char zip[512];
	snprintf(zip, sizeof(zip), "%s", path);
	char *const list[] = {"unzip", "-l", zip, NULL};
	int ok = process_run(list, NULL, zip_line, &z) == 0 && image_files(z.names, z.count);
	for (int i = 0; i < z.count; ++i)
		free(z.names[i]);
	return ok;
}

static int for_box(const char *name)
{
	char box[64];
	char machine[64];
	char model[64];
	boxinfo_box_name(box, sizeof(box));
	boxinfo_value("machinebuild", machine, sizeof(machine));
	boxinfo_value("model", model, sizeof(model));
	return (box[0] && strstr(name, box)) || (machine[0] && strstr(name, machine)) || (model[0] && strstr(name, model));
}

static void scan_zips(const char *dir, struct image *images, int *count)
{
	DIR *d = opendir(dir);
	const struct dirent *entry;
	if (!d)
		return;
	while ((entry = readdir(d)) && *count < MAX_IMAGES) {
		const char *name = entry->d_name;
		size_t length = strlen(name);
		struct image *image = &images[*count];
		struct stat info;
		if (name[0] == '.' || length < 5 || strcmp(name + length - 4, ".zip") || !for_box(name))
			continue;
		memset(image, 0, sizeof(*image));
		snprintf(image->link, sizeof(image->link), "%s/%.255s", dir, name);
		if (stat(image->link, &info) != 0 || !S_ISREG(info.st_mode) || !valid_zip(image->link))
			continue;
		snprintf(image->name, sizeof(image->name), "%.159s", name);
		snprintf(image->category, sizeof(image->category), "%s", strstr(name, "backup") ? N_("Backup images") :
			N_("Local images"));  /* Translated where they are shown. */
		image->size = info.st_size;
		image->local = 1;
		(*count)++;
	}
	closedir(d);
}

/* Like getImagesList() of the FlashManager: unzipped images of before only fill the media. */
static void remove_unzipped(const char *folder)
{
	DIR *d = opendir(folder);
	struct dirent *entry;
	if (!d)
		return;
	while ((entry = readdir(d))) {
		char path[600];
		struct stat info;
		size_t length = strlen(entry->d_name);
		if (length <= 9 || strcmp(entry->d_name + length - 9, ".unzipped"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode))
			remove_tree(path);
	}
	closedir(d);
}

static void scan_media(const char *media, struct image *images, int *count)
{
	static const char *const folders[] = {"images", "downloaded_images", "imagebackups"};
	struct stat parent;
	if (stat(media, &parent) != 0 || !S_ISDIR(parent.st_mode))
		return;
	scan_zips(media, images, count);
	for (size_t i = 0; i < 3; ++i) {
		char path[320];
		struct stat info;
		snprintf(path, sizeof(path), "%.290s/%s", media, folders[i]);
		if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode) && info.st_dev == parent.st_dev) {  /* No link, no mount. */
			remove_unzipped(path);
			scan_zips(path, images, count);
		}
	}
}

/* The zips of this box on all media, the FlashManager lists them only for openATV. */
static void local_images(struct image *images, int *count)
{
	static const char *const bases[] = {"/media", "/media/net"};
	for (size_t b = 0; b < 2; ++b) {
		DIR *d = opendir(bases[b]);
		const struct dirent *entry;
		if (!d)
			continue;
		while ((entry = readdir(d))) {
			char path[300];
			const char *name = entry->d_name;
			if (name[0] == '.' || !strcmp(name, "autofs") || !strcmp(name, "audiocd") || !strcmp(name, "mmc") ||
				(!b && !strcmp(name, "net")))
				continue;
			snprintf(path, sizeof(path), "%s/%.255s", bases[b], name);
			scan_media(path, images, count);
		}
		closedir(d);
	}
}

/* A small JSON reader for the feeds: {"category": {"file": {"link": ..., "name": ..., "size": ...}}}. */
struct json {
	const char *p;
	const char *end;
};

static void skip_space(struct json *j)
{
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n'))
		j->p++;
}

static int eat(struct json *j, char c)
{
	skip_space(j);
	if (j->p < j->end && *j->p == c) {
		j->p++;
		return 1;
	}
	return 0;
}

static void put_utf8(char *out, size_t size, size_t *used, unsigned int code)
{
	char bytes[4];
	int n = 0;
	if (code < 0x80)
		bytes[n++] = (char)code;
	else if (code < 0x800) {
		bytes[n++] = (char)(0xc0 | (code >> 6));
		bytes[n++] = (char)(0x80 | (code & 0x3f));
	} else {
		bytes[n++] = (char)(0xe0 | (code >> 12));
		bytes[n++] = (char)(0x80 | ((code >> 6) & 0x3f));
		bytes[n++] = (char)(0x80 | (code & 0x3f));
	}
	for (int i = 0; i < n && *used + 1 < size; ++i)
		out[(*used)++] = bytes[i];
}

static char unescape(char c)  /* The character of "\c". */
{
	switch (c) {
	case 'n':
		return '\n';
	case 't':
		return '\t';
	case 'r':
		return '\r';
	case 'b':
		return '\b';
	case 'f':
		return '\f';
	default:
		return c;
	}
}

static int read_string(struct json *j, char *out, size_t size)
{
	size_t used = 0;
	if (!eat(j, '"'))
		return 0;
	while (j->p < j->end && *j->p != '"') {
		char c = *j->p++;
		if (c == '\\' && j->p < j->end) {
			c = *j->p++;
			if (c == 'u' && j->end - j->p >= 4) {
				char hex[5] = {j->p[0], j->p[1], j->p[2], j->p[3], 0};
				put_utf8(out, size, &used, (unsigned int)strtoul(hex, NULL, 16));
				j->p += 4;
				continue;
			}
			c = unescape(c);
		}
		if (used + 1 < size)
			out[used++] = c;
	}
	out[used] = '\0';
	return eat(j, '"');
}

static int skip_value(struct json *j, int depth)
{
	char scratch[8];
	skip_space(j);
	if (j->p >= j->end || depth > 32)
		return 0;
	if (*j->p == '"')
		return read_string(j, scratch, sizeof(scratch));
	if (*j->p == '{' || *j->p == '[') {
		char close = *j->p == '{' ? '}' : ']';
		int object = *j->p == '{';
		j->p++;
		if (eat(j, close))
			return 1;
		do {
			if (object && (!read_string(j, scratch, sizeof(scratch)) || !eat(j, ':')))
				return 0;
			if (!skip_value(j, depth + 1))
				return 0;
		} while (eat(j, ','));
		return eat(j, close);
	}
	while (j->p < j->end && !strchr(",}] \t\r\n", *j->p))  /* A number, true, false or null. */
		j->p++;
	return 1;
}

static int read_scalar(struct json *j, char *out, size_t size)  /* A string or a number as text. */
{
	skip_space(j);
	if (j->p < j->end && *j->p == '"')
		return read_string(j, out, size);
	{
		const char *start = j->p;
		if (!skip_value(j, 0))
			return 0;
		snprintf(out, size, "%.*s", (int)(j->p - start), start);
		return 1;
	}
}

static int read_image(struct json *j, const char *category, const char *file, struct image *image)
{
	memset(image, 0, sizeof(*image));
	snprintf(image->category, sizeof(image->category), "%s", category);
	snprintf(image->name, sizeof(image->name), "%s", file);
	if (!eat(j, '{'))
		return skip_value(j, 1) ? 0 : -1;
	if (eat(j, '}'))
		return 0;
	do {
		char key[32];
		char value[512];
		if (!read_string(j, key, sizeof(key)) || !eat(j, ':'))
			return -1;
		skip_space(j);
		if (j->p < j->end && (*j->p == '{' || *j->p == '[')) {
			if (!skip_value(j, 2))
				return -1;
			continue;
		}
		if (!read_scalar(j, value, sizeof(value)))
			return -1;
		if (!strcmp(key, "link"))
			snprintf(image->link, sizeof(image->link), "%s", value);
		else if (!strcmp(key, "name") && value[0])
			snprintf(image->name, sizeof(image->name), "%.159s", value);
		else if (!strcmp(key, "size"))
			image->size = strtoll(value, NULL, 10);
	} while (eat(j, ','));
	if (!eat(j, '}'))
		return -1;
	return image->link[0] ? 1 : 0;
}

/* The images of one category, 0 for a broken feed. */
static int parse_category(struct json *j, const char *category, struct image *images, int *count)
{
	if (!eat(j, '{'))
		return skip_value(j, 1);
	if (eat(j, '}'))
		return 1;
	do {
		char file[160];
		int result;
		if (!read_string(j, file, sizeof(file)) || !eat(j, ':'))
			return 0;
		result = read_image(j, category, file, *count < MAX_IMAGES ? &images[*count] : &images[MAX_IMAGES]);
		if (result < 0)
			return 0;
		if (result > 0 && *count < MAX_IMAGES)
			(*count)++;
	} while (eat(j, ','));
	return eat(j, '}');
}

static int parse_feed(const char *text, size_t length, struct image *images, int *count)
{
	struct json j = {text, text + length};
	if (length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3))
		j.p += 3;
	if (!eat(&j, '{'))
		return 0;
	if (eat(&j, '}'))
		return 1;
	do {
		char category[96];
		if (!read_string(&j, category, sizeof(category)) || !eat(&j, ':'))
			return 0;
		if (!parse_category(&j, category, images, count))
			return 0;
	} while (eat(&j, ','));
	return 1;
}

static int download_file(const char *url, const char *path, int seconds)
{
	char tool[64];
	char timeout[16];
	char target[256];
	char source[256];
	snprintf(timeout, sizeof(timeout), "%d", seconds);
	snprintf(target, sizeof(target), "%s", path);
	snprintf(source, sizeof(source), "%s", url);
	if (process_find("curl", tool, sizeof(tool))) {
		char *const curl[] = {tool, "-fsSL", "-A", USER_AGENT, "--max-time", timeout, "-o", target, source, NULL};
		return run_quiet(curl) == 0;
	}
	if (process_find("wget", tool, sizeof(tool))) {
		char *const wget[] = {tool, "-q", "-U", USER_AGENT, "-T", timeout, "-O", target, source, NULL};
		return run_quiet(wget) == 0;
	}
	return 0;
}

static char *read_file(const char *path, size_t *length)
{
	FILE *file = fopen(path, "rb");
	char *text = NULL;
	long size = -1;
	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0)
		size = ftell(file);
	if (size >= 0 && size < 16 * MB && fseek(file, 0, SEEK_SET) == 0)
		text = malloc((size_t)size + 1);
	if (text) {
		*length = fread(text, 1, (size_t)size, file);
		if (*length > (size_t)size)
			*length = (size_t)size;
		text[*length] = '\0';
	}
	fclose(file);
	return text;
}

static int feed_images(const struct feed *feed, struct image *images, int *count)
{
	size_t length = 0;
	char *text = NULL;
	int ok;
	unlink(FEED_FILE);
	if (download_file(feed->url, FEED_FILE, 30))
		text = read_file(FEED_FILE, &length);
	if (!text) {
		unlink(FEED_FILE);
		return 0;
	}
	ok = parse_feed(text, length, images, count);
	free(text);
	unlink(FEED_FILE);
	return ok;
}

/* keyDistribution() of the FlashManager: openATV and the list of OpenATV/FlashImage for the box. */
static int feed_before(const struct feed *a, const struct feed *b, const char *distro)
{
	if (!strcasecmp(a->name, distro) != !strcasecmp(b->name, distro))
		return !strcasecmp(a->name, distro);
	return strcasecmp(a->name, b->name) < 0;
}

static int find_feed(const struct feed *feeds, int count, const char *name)
{
	for (int i = 0; i < count; ++i)
		if (!strcasecmp(feeds[i].name, name))
			return i;
	return -1;
}

/* OpenATV, the feed with the images of every box. */
static void default_feed(struct feed *feed)
{
	char box[64];
	boxinfo_box_name(box, sizeof(box));
	strcpy(feed->name, "OpenATV");
	snprintf(feed->url, sizeof(feed->url), OPENATV_FEED "%s", box);
}

/* The index of the default feed, which load_feeds() always lists; 0 only guards against -1. */
static int default_feed_index(const struct feed *feeds, int count)
{
	int found = find_feed(feeds, count, "OpenATV");
	return found < 0 ? 0 : found;
}

/* Adds the feeds of the list to the count ones, returns the new count. */
static int read_distributions(const char *text, size_t length, struct feed *feeds, int count)
{
	struct json j = {text, text + length};  /* [["OpenPLi", "http://..."], ...] */
	if (!eat(&j, '[') || eat(&j, ']'))
		return count;
	do {
		struct feed f;
		if (!eat(&j, '[') || !read_string(&j, f.name, sizeof(f.name)) || !eat(&j, ',') ||
			!read_string(&j, f.url, sizeof(f.url)) || !eat(&j, ']'))
			break;
		if (count < MAX_FEEDS && strcasecmp(f.name, "OpenATV"))
			feeds[count++] = f;
	} while (eat(&j, ','));
	return count;
}

/* The running distribution first, the others in alphabetical order. */
static int load_feeds(struct feed *feeds, const char *distro)
{
	char machine[64];
	char url[256];
	size_t length = 0;
	char *text = NULL;
	int count = 1;
	boxinfo_value("machinebuild", machine, sizeof(machine));
	default_feed(&feeds[0]);
	snprintf(url, sizeof(url), DISTRIBUTIONS "%s.json", machine);
	unlink(FEED_FILE);
	if (download_file(url, FEED_FILE, 10))
		text = read_file(FEED_FILE, &length);
	if (text) {
		count = read_distributions(text, length, feeds, count);
		free(text);
	}
	unlink(FEED_FILE);
	for (int i = 1; i < count; ++i) {
		struct feed f = feeds[i];
		int j = i;
		for (; j > 0 && feed_before(&f, &feeds[j - 1], distro); --j)
			feeds[j] = feeds[j - 1];
		feeds[j] = f;
	}
	return count;
}

static int newest_first(const void *a, const void *b)
{
	const struct image *x = a;
	const struct image *y = b;
	int order;
	if (x->local != y->local)
		return y->local - x->local;
	if ((order = strcmp(y->category, x->category)))
		return order;
	return strcmp(y->name, x->name);
}

struct listing {
	struct image *images;  /* MAX_IMAGES + 1, the last one for parsing only. */
	int count;
	char **items;  /* Rows: a category, then its images. */
	int *rows;  /* The image of a row, -2 - its first image for a category, -1 for none. */
	char *marks;
	char expanded[MAX_IMAGES + 1];  /* By the first image of a category, all start collapsed. */
	int row_count;
	int feed_ok;
};

static void free_rows(struct listing *l)
{
	for (int i = 0; i < l->row_count; ++i)
		free(l->items[i]);
	free(l->items);
	free(l->rows);
	free(l->marks);
	l->items = NULL;
	l->rows = NULL;
	l->marks = NULL;
	l->row_count = 0;
}

/* The row of the category that starts with image first. */
static void add_category_row(struct listing *l, int first)
{
	const struct image *image = &l->images[first];
	int end = first + 1;
	char count[48];
	while (end < l->count && l->images[end].local == image->local && !strcmp(l->images[end].category, image->category))
		end++;
	snprintf(count, sizeof(count), ngettext("%d image", "%d images", end - first), end - first);
	if (asprintf(&l->items[l->row_count], "%s  %s\t%s", l->expanded[first] ? "-" : "+",
		image->local ? _(image->category) : image->category, count) < 0)
		l->items[l->row_count] = NULL;
	l->rows[l->row_count++] = -2 - first;
}

static void build_rows(struct listing *l)
{
	int max = l->count * 2 + 1;
	int first = 0;
	free_rows(l);
	l->items = calloc((size_t)max, sizeof(*l->items));
	l->rows = calloc((size_t)max, sizeof(*l->rows));
	l->marks = calloc((size_t)max, sizeof(*l->marks));
	if (!l->items || !l->rows || !l->marks)
		return;
	for (int i = 0; i < l->count; ++i) {
		const struct image *image = &l->images[i];
		char size[32] = "";
		if (!i || image->local != image[-1].local || strcmp(image->category, image[-1].category)) {
			first = i;
			add_category_row(l, first);
		}
		if (!l->expanded[first])
			continue;
		if (image->size > 0)
			snprintf(size, sizeof(size), "%lld MB", (image->size + MB / 2) / MB);
		if (asprintf(&l->items[l->row_count], "      %s\t%s", image->name, size) < 0)
			l->items[l->row_count] = NULL;
		l->rows[l->row_count++] = i;
	}
	if (!l->row_count) {
		l->items[0] = strdup(_("No images found"));
		l->rows[0] = -1;
		l->marks[0] = 2;
		l->row_count = 1;
	}
}

static void load_images(const struct ui_context *ui, struct listing *l, const struct feed *feed)
{
	char text[128];
	snprintf(text, sizeof(text), _("Loading the images of %s."), feed->name);
	ui_progress(ui, TITLE, text, 20, _("Please wait..."), _("Please wait..."));
	l->count = 0;
	l->feed_ok = feed_images(feed, l->images, &l->count);
	if (!l->feed_ok)
		l->count = 0;
	ui_progress(ui, TITLE, _("Looking for images on the media."), 70, _("Please wait..."), _("Please wait..."));
	local_images(l->images, &l->count);
	qsort(l->images, (size_t)l->count, sizeof(*l->images), newest_first);
	memset(l->expanded, 0, sizeof(l->expanded));
	build_rows(l);
}

static int choose_feed(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct feed *feeds, int count, int current)
{
	const char *items[MAX_FEEDS];
	char footer[128];
	char title[128];
	int selected = current;
	for (int i = 0; i < count; ++i)
		items[i] = feeds[i].name;
	while (!(stop && *stop)) {
		enum input_key key;
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("Choose"),
			.back = _("Images")});
		snprintf(title, sizeof(title), "%s - %s", TITLE, _("Distribution"));
		ui_menu_marked(ui, &(struct ui_menu){.title = title,
			.body = _("Which distribution should the images come from?"), .items = items, .count = count,
			.selected = selected, .marked = current, .footer = footer});
		key = input_next(input, 1000);
		selected = list_move(key, selected, count);
		if (key == INPUT_OK)
			return selected;
		else if (key == INPUT_BACK)
			return current;
	}
	return current;
}

static void encode_spaces(const char *link, char *url, size_t size)  /* OpenSPA has spaces in its links. */
{
	size_t used = 0;
	const char *c = link;
	while (*c && used + 4 < size) {
		if (*c == ' ') {
			memcpy(url + used, "%20", 3);
			used += 3;
		} else
			url[used++] = *c;
		++c;
	}
	url[used] = '\0';
}

static pid_t start_download(const char *tool, int curl, const char *part, const char *url)
{
	pid_t pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
		}
		if (curl)
			execl(tool, tool, "-fsSL", "-A", USER_AGENT, "--connect-timeout", "30", "-o", part, url, (char *)NULL);  /* NOSONAR the link of the image chosen from the feed */
		else
			execl(tool, tool, "-q", "-U", USER_AGENT, "-T", "30", "-O", part, url, (char *)NULL);  /* NOSONAR the link of the image chosen from the feed */
		_exit(127);
	}
	return pid;
}

/* The progress until the download ends, -1 when it was stopped. */
static int wait_download(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct image *image, const char *part, pid_t pid, int *status)
{
	char body[256];
	snprintf(body, sizeof(body), _("Downloading %s"), image->name);
	ui_busy(ui, 1);
	for (;;) {
		struct stat info;
		long long got = stat(part, &info) == 0 ? info.st_size : 0;
		char detail[96];
		enum input_key key;
		if (waitpid(pid, status, WNOHANG) == pid)
			break;
		char footer[64];
		if (image->size > 0)
			snprintf(detail, sizeof(detail), _("%lld of %lld MB"), got / MB, (image->size + MB / 2) / MB);
		else
			snprintf(detail, sizeof(detail), _("%lld MB"), got / MB);
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.red = _("Cancel")});
		ui_progress(ui, TITLE, body, image->size > 0 ? (int)(got * 100 / image->size) : 0, detail, footer);
		key = input_next(input, 250);
		if (key == INPUT_RED || key == INPUT_BACK || (stop && *stop)) {
			kill(pid, SIGTERM);
			waitpid(pid, status, 0);
			unlink(part);
			ui_busy(ui, 0);
			return -1;
		}
	}
	ui_busy(ui, 0);
	return 0;
}

/* The size of the file just downloaded, not of a path checked again. */
static int has_size(const char *part, long long size)
{
	struct stat info;
	int fd = open(part, O_RDONLY | O_CLOEXEC);
	int complete = fd >= 0 && fstat(fd, &info) == 0 && info.st_size == size;
	if (fd >= 0)
		close(fd);
	return complete;
}

/* A download with progress, BACK stops it. */
static int fetch_image(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct image *image, const char *path)
{
	char url[640];
	char part[620];
	char tool[64];
	pid_t pid;
	int status = -1;
	int curl = process_find("curl", tool, sizeof(tool));
	if (!curl && !process_find("wget", tool, sizeof(tool)))
		return 0;
	encode_spaces(image->link, url, sizeof(url));
	snprintf(part, sizeof(part), "%s.part", path);
	unlink(part);
	pid = start_download(tool, curl, part, url);
	if (pid < 0)
		return 0;
	if (wait_download(ui, input, stop, image, part, pid, &status) < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		unlink(part);
		return 0;
	}
	if (image->size > 0 && !has_size(part, image->size)) {
		unlink(part);
		return 0;
	}
	return rename(part, path) == 0;
}

struct unzip_state {
	const struct ui_context *ui;
	char body[256];
	char last[160];
	int files;
};

static void unzip_line(const char *line, void *opaque)
{
	struct unzip_state *u = opaque;
	const char *name = strstr(line, "inflating: ");
	if (!name)
		name = strstr(line, "extracting: ");
	if (!name)
		return;
	name = strchr(name, ':') + 2;
	snprintf(u->last, sizeof(u->last), "%.159s", strrchr(name, '/') ? strrchr(name, '/') + 1 : name);
	u->files++;
}

static void unzip_tick(void *opaque)
{
	const struct unzip_state *u = opaque;
	ui_progress(u->ui, TITLE, u->body, u->files * 100 / 6, u->last[0] ? u->last : _("Please wait..."),
		_("Please wait..."));
}

/* The folders in d; the names of up to 64 other entries go to names. */
static int list_entries(DIR *d, const char *dir, char *names[], int *count)
{
	const struct dirent *entry;
	int subdirs = 0;
	while ((entry = readdir(d))) {
		char path[1024];
		struct stat info;
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%.255s", dir, entry->d_name);
		if (lstat(path, &info) != 0)
			continue;
		if (S_ISDIR(info.st_mode))
			subdirs++;
		else if (*count < 64)
			names[(*count)++] = strdup(entry->d_name);
	}
	return subdirs;
}

static int find_image_dir(const char *dir, char *found, size_t size, int depth);

static int find_in_subdirs(DIR *d, const char *dir, char *found, size_t size, int depth)
{
	const struct dirent *entry;
	rewinddir(d);
	while ((entry = readdir(d))) {
		char path[1024];
		struct stat info;
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%.255s", dir, entry->d_name);
		if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode) && find_image_dir(path, found, size, depth + 1))
			return 1;
	}
	return 0;
}

/* findImageFiles() of the FlashManager: the first folder without folders that holds an image. */
static int find_image_dir(const char *dir, char *found, size_t size, int depth)
{
	DIR *d = opendir(dir);
	char *names[64];
	int count = 0;
	int subdirs;
	int ok = 0;
	if (!d)
		return 0;
	subdirs = list_entries(d, dir, names, &count);
	if (!subdirs && count) {
		ok = image_files(names, count);
		if (ok)
			snprintf(found, size, "%s", dir);
	} else if (subdirs && depth < 4)
		ok = find_in_subdirs(d, dir, found, size, depth);
	for (int i = 0; i < count; ++i)
		free(names[i]);
	closedir(d);
	return ok;
}

static void drop_tar_beside_ubi(const char *dir)  /* startUnzip() of the FlashManager */
{
	char ubi[1100];
	char tar[1100];
	snprintf(ubi, sizeof(ubi), "%s/rootfs.ubi", dir);
	snprintf(tar, sizeof(tar), "%s/rootfs.tar.bz2", dir);
	if (access(ubi, F_OK) == 0)
		unlink(tar);
}

static int unzip_image(const struct ui_context *ui, const char *zip, const char *dir, const char *name)
{
	struct unzip_state u = {.ui = ui};
	char source[600];
	char target[600];
	snprintf(source, sizeof(source), "%s", zip);
	snprintf(target, sizeof(target), "%s", dir);
	char *const unzip[] = {"unzip", "-o", source, "-d", target, NULL};
	remove_tree(dir);  /* Only a leftover of before is removed, ofgwrite may still read the new one. */
	if (mkdir(dir, 0755) != 0)
		return 0;
	snprintf(u.body, sizeof(u.body), _("Unzipping %s"), name);
	unzip_tick(&u);
	return process_run_with_updates(unzip, NULL, unzip_line, unzip_tick, 200, &u) == 0;
}

struct kept_lines {
	char *text[400];
	int count;
};

static void keep_line(const char *line, void *opaque)
{
	struct kept_lines *lines = opaque;
	if (lines->count < 400)
		lines->text[lines->count++] = strdup(line);
}

struct check_state {
	const struct ui_context *ui;
	struct kept_lines lines;
};

static void check_line(const char *line, void *opaque)
{
	keep_line(line, &((struct check_state *)opaque)->lines);
}

static void check_tick(void *opaque)  /* Again and again, ofgwrite clears the whole screen when it starts. */
{
	char title[128];
	snprintf(title, sizeof(title), "%s - %s", TITLE, _("Check"));
	ui_embed(((struct check_state *)opaque)->ui, title, _("Please wait, ofgwrite checks the image..."),
		OFGWRITE_WIDTH, OFGWRITE_HEIGHT);
}

/* The command line of ofgwrite, with -n for the check. */
struct ofgwrite_command {
	char args[MAX_ARGS][80];
	char dir[1024];
	char *argv[MAX_ARGS + 4];
};

static void ofgwrite_command(struct ofgwrite_command *c, const struct target *t, const char *dir, int check)
{
	int argc = 0;
	c->argv[argc++] = OFGWRITE;
	if (check)
		c->argv[argc++] = "-n";
	for (int i = 0; i < t->arg_count; ++i) {
		memcpy(c->args[i], t->args[i], sizeof(c->args[i]));
		c->argv[argc++] = c->args[i];
	}
	snprintf(c->dir, sizeof(c->dir), "%s", dir);
	c->argv[argc++] = c->dir;
	c->argv[argc] = NULL;
}

static int run_check(struct ui_context *ui, const struct target *t, const char *dir, struct live_output *output)
{
	static struct check_state state;
	struct ofgwrite_command command;
	int result;
	ofgwrite_command(&command, t, dir, 1);
	memset(output, 0, sizeof(*output));
	output->ui = ui;
	output->title = NULL;
	state.ui = ui;
	state.lines.count = 0;
	check_tick(&state);
	/* ofgwrite draws its window also with -n, ORM draws around it. */
	result = process_run_with_updates(command.argv, NULL, check_line, check_tick, 200, &state);
	ui_close(ui);  /* ofgwrite changed the mode and ended the manual blit. */
	ui_open(ui);
	for (int i = 0; i < state.lines.count; ++i) {
		live_output_add(output, state.lines.text[i]);
		free(state.lines.text[i]);
	}
	return result;
}

/* "3 (linuxrootfs3 on mmcblk0p23), the running image", for the table of the check. */
static void slot_text(const struct target *t, char *text, size_t size)
{
	if (!t->multiboot)
		snprintf(text, size, "%s", _("the running image"));
	else if (t->slot.rootsubdir[0])
		snprintf(text, size, _("%s (%s on %s), the running image"), t->slot.code, t->slot.rootsubdir,
			short_device(t->slot.device));
	else
		snprintf(text, size, _("%s (%s), the running image"), t->slot.code, short_device(t->slot.device));
}

static void add_log_lines(struct live_output *output, char *text)  /* Every line as it is, empty ones too. */
{
	char *rest = text;
	while (rest) {
		const char *line = strsep(&rest, "\n");
		if (rest || *line)  /* Not the empty rest after the last newline. */
			live_output_add(output, line);
	}
}

static void show_flash_log(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct live_output *output, int ok)
{
	int first = output->count;  /* The end, text_view stops at the last page. */
	char note[160];
	char footer[320];
	enum input_key key;
	if (ok)
		snprintf(note, sizeof(note), "%s", _("Flashing finished, restart the receiver in the menu."));
	else
		snprintf(note, sizeof(note), _("Flashing failed, see %s."), FLASH_LOG);
	snprintf(footer, sizeof(footer), "%s   ARROWS: %s   OK: %s", note, _("Scroll"), _("Menu"));
	do
		key = text_view(ui, input, stop, &(struct text_page){.title = output->title, .lines = output->lines,
			.count = output->count, .first = &first, .empty = _("ofgwrite wrote nothing."), .footer = footer});
	while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED && key != INPUT_NONE);
}

/* Like the fbClass lock of the FlashManager the screen belongs to ofgwrite: it stops enigma2.sh and
 * with it ORM by init 2, flashes from a new root and restarts the receiver. */
static void flash_now(struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct target *t, const char *dir)
{
	struct live_output output;
	struct ofgwrite_command command;
	char title[128];
	char *text;
	size_t length = 0;
	int status = -1;
	int daemonized;
	pid_t pid;
	ofgwrite_command(&command, t, dir, 0);
	remove_restore_flags();  /* A clean flash, the wizard offers the restore. */
	sync();
	ui_close(ui);
	pid = fork();
	if (pid == 0) {
		int log = open(FLASH_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);  /* In /tmp, ofgwrite takes it along to the new root. */
		int null = open("/dev/null", O_RDONLY);
		if (null >= 0)
			dup2(null, STDIN_FILENO);
		if (log >= 0) {
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		execv(OFGWRITE, command.argv);  /* NOSONAR the ofgwrite command of the FlashManager */
		_exit(127);
	}
	while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	text = read_file(FLASH_LOG, &length);
	daemonized = text && strstr(text, "daemonize");
	if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && daemonized) {
		free(text);
		/* ofgwrite goes on alone and restarts the receiver. Its init 2 ends ORM, which must not
		 * return and clean up then: ofgwrite still reads the unzipped image. */
		for (;;) {
			input_next(input, 1000);
			if (stop && *stop)
				_exit(0);
		}
	}
	ui_open(ui);
	memset(&output, 0, sizeof(output));
	output.ui = ui;
	snprintf(title, sizeof(title), "%s - %s", TITLE, _("Flashing"));
	output.title = title;
	if (text)
		add_log_lines(&output, text);
	free(text);
	show_flash_log(ui, input, stop, &output, pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	live_output_free(&output);
}

/* The image is unzipped; OK flashes only in the output of a successful check with ofgwrite -n. */
/* Whether ofgwrite flashes the kernel too, it does only with -k. */
static void kernel_text(const struct target *t, char *text, size_t size)
{
	for (int i = 0; i < t->arg_count; ++i) {
		const char *device = t->args[i] + 2;
		if (strncmp(t->args[i], "-k", 2))
			continue;
		if (!strcmp(t->model, "dm820") || !strcmp(t->model, "dm7080"))
			snprintf(text, size, "%s", _("flashed too, kernel A of all slots"));
		else if (*device)
			snprintf(text, size, _("flashed too (%s)"), device);
		else if (t->multiboot && t->slot.kernel[0])
			snprintf(text, size, _("flashed too (%s)"), short_device(t->slot.kernel));
		else if (!t->multiboot && t->mtdkernel[0])
			snprintf(text, size, _("flashed too (%s)"), t->mtdkernel);
		else
			snprintf(text, size, "%s", _("flashed too"));
		return;
	}
	snprintf(text, size, "%s", _("stays as it is"));
}

static void confirm_rows(const struct target *t, const struct image *image, const char *source, const char *dir,
	char rows[5][640])
{
	char slot[160];
	char kernel[96];
	slot_text(t, slot, sizeof(slot));
	snprintf(rows[0], sizeof(rows[0]), "%s\t%s", _("Image"), image->name);
	if (image->local) {
		const char *slash = strrchr(image->link, '/');
		snprintf(rows[1], sizeof(rows[1]), "%s\t%.*s", _("Location"), slash ? (int)(slash - image->link) : 0,
			image->link);
	} else
		snprintf(rows[1], sizeof(rows[1]), "%s\t%s", _("Location"), source);
	snprintf(rows[2], sizeof(rows[2]), "%s\t%s", _("Slot"), slot);
	kernel_text(t, kernel, sizeof(kernel));
	snprintf(rows[3], sizeof(rows[3]), "%s\t%s", _("Kernel"), kernel);
	snprintf(rows[4], sizeof(rows[4]), "%s\tofgwrite", _("Command"));
	for (int i = 0; i < t->arg_count; ++i)
		snprintf(rows[4] + strlen(rows[4]), sizeof(rows[4]) - strlen(rows[4]), " %s", t->args[i]);
	snprintf(rows[4] + strlen(rows[4]), sizeof(rows[4]) - strlen(rows[4]), " %s", dir);
}

/* The check with ofgwrite -n, after a successful one OK flashes; 1 when it flashed. */
static int check_image(struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct target *t, const char *dir)
{
	struct live_output output;
	int ok = run_check(ui, t, dir, &output) == 0;
	int first = output.count;  /* The end, text_view stops at the last page. */
	char title[256];
	char keys[160];
	enum input_key key;
	snprintf(title, sizeof(title), "%s - %s\t%s - %s", TITLE, _("Check"), ok ? UI_GREEN : UI_RED,
		ok ? _("The check was successful") : _("The check failed"));
	if (ok)
		ui_keys(keys, sizeof(keys), &(struct ui_key_names){.arrows = _("Scroll"), .ok = _("Flash now"),
			.back = _("Back")});
	else
		ui_keys(keys, sizeof(keys), &(struct ui_key_names){.arrows = _("Scroll"), .ok = _("Back")});
	do
		key = text_view(ui, input, stop, &(struct text_page){.title = title, .lines = output.lines,
			.count = output.count, .first = &first, .empty = _("ofgwrite wrote nothing."), .footer = keys});
	while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_NONE);
	live_output_free(&output);
	if (ok && key == INPUT_OK && ask(ui, input, stop, TITLE, _("Flash the image now? The running image is "
		"replaced and the receiver restarts. Do not switch it off while it flashes."), 0)) {
		flash_now(ui, input, stop, t, dir);
		return 1;
	}
	return 0;
}

static void confirm(struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct target *t, const struct image *image, const char *source, const char *dir)
{
	char rows[5][640];
	const char *items[5];
	char footer[160];
	for (int i = 0; i < 5; ++i)
		items[i] = rows[i];
	while (!(stop && *stop)) {
		char body[512];
		enum input_key key;
		confirm_rows(t, image, source, dir, rows);
		snprintf(body, sizeof(body), "%s\n%s", _("Clean flash: all settings are lost, after the restart the most recent "
			"backup can be restored."), _("The check only tests the image and writes nothing. After a "
			"successful check, OK starts flashing."));
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.ok = _("Check"), .back = _("Images")});
		ui_menu_table(ui, &(struct ui_menu){.title = TITLE, .body = body, .items = items, .count = 5, .selected = -1,
			.marked = -1, .footer = footer});
		key = input_next(input, 1000);
		if (key == INPUT_BACK)
			return;
		if (key != INPUT_OK)
			continue;
		if (check_image(ui, input, stop, t, dir))
			return;
	}
}

static void prepare(struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct target *t, const struct image *image, const char *feed, const char *media)
{
	char folder[300];
	char zip[600];
	char dir[600];
	char found[1024];
	char base[160];
	struct statvfs fs;
	long long need;
	size_t length;
	snprintf(folder, sizeof(folder), "%s/images", media);
	snprintf(base, sizeof(base), "%s", image->name);
	length = strlen(base);
	if (length > 4 && !strcasecmp(base + length - 4, ".zip"))
		base[length - 4] = '\0';
	snprintf(dir, sizeof(dir), "%s/%s.unzipped", folder, base);
	if (image->local)
		snprintf(zip, sizeof(zip), "%s", image->link);
	else
		snprintf(zip, sizeof(zip), "%s/%s", folder, image->name);
	mkdir(folder, 0755);
	need = (image->size > 0 ? image->size : 500 * MB) * (image->local ? 1 : 2) + 50 * MB;
	if (statvfs(media, &fs) == 0 && (long long)fs.f_bavail * (long long)fs.f_frsize < need) {
		char text[256];
		snprintf(text, sizeof(text), _("There is not enough free space on %s, it needs %lld MB."), media, need / MB);
		message(ui, input, stop, text);
		return;
	}
	if (!image->local) {
		struct stat info;
		if (!(image->size > 0 && stat(zip, &info) == 0 && info.st_size == image->size)) {  /* Downloaded before. */
			int result = fetch_image(ui, input, stop, image, zip);
			if (result < 0)
				return;
			if (!result) {
				message(ui, input, stop, _("The image could not be downloaded."));
				return;
			}
		}
	}
	if (!unzip_image(ui, zip, dir, image->name) || !find_image_dir(dir, found, sizeof(found), 0)) {
		message(ui, input, stop, _("The image could not be unzipped or holds no image for ofgwrite."));
		return;
	}
	drop_tar_beside_ubi(found);
	confirm(ui, input, stop, t, image, image->local ? image->link : feed, found);
}

/* The feeds at the start, current gets the one of the running distribution; returns their count. */
static int start_feeds(const struct ui_context *ui, struct feed *feeds, const char *distro, int *current)
{
	int feed_count;
	if (strcasecmp(distro, "openatv") && distro[0]) {  /* The feed of the running distribution. */
		ui_progress(ui, TITLE, _("Loading the list of distributions."), 10, _("Please wait..."), _("Please wait..."));
		feed_count = load_feeds(feeds, distro);
		*current = find_feed(feeds, feed_count, distro);
		if (*current < 0)
			*current = default_feed_index(feeds, feed_count);
	} else {
		default_feed(&feeds[0]);
		feed_count = 1;
	}
	return feed_count;
}

static const char *ok_label(const struct listing *l, int selected)
{
	if (l->rows[selected] >= 0)
		return _("Choose");
	if (l->rows[selected] <= -2 && l->expanded[-2 - l->rows[selected]])
		return _("Close");
	return _("Open");
}

static void listing_body(const struct target *t, const struct listing *l, const char *media, char *body, size_t size)
{
	char slot[160];
	if (!t->multiboot)
		snprintf(slot, sizeof(slot), "%s", _("The chosen image replaces the running image."));
	else if (t->slot.rootsubdir[0])
		snprintf(slot, sizeof(slot), _("The chosen image replaces the running image in slot %s (%s on %s)."),
			t->slot.code, t->slot.rootsubdir, short_device(t->slot.device));
	else
		snprintf(slot, sizeof(slot), _("The chosen image replaces the running image in slot %s (%s)."),
			t->slot.code, short_device(t->slot.device));
	snprintf(body, size, strstr(media, "/usb") ? _("%s%s Downloaded images are saved on the USB stick "
		"(%s/images).") : _("%s%s Downloaded images are saved on the hard disk (%s/images)."),
		l->feed_ok ? "" : _("The images of the distribution could not be loaded. "), slot, media);
}

static int move_selection(const struct listing *l, enum input_key key, int selected)
{
	int step = key == INPUT_UP || key == INPUT_LEFT ? -1 : 1;
	int next = list_move(key, selected, l->row_count);
	while (l->rows[next] == -1 && next != selected)  /* Rows that cannot be chosen, in the direction of the key. */
		next = (next + step + l->row_count) % l->row_count;
	return next;
}

static int pick_feed(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	struct feed *feeds, int *feed_count, int *current, const char *distro)
{
	if (*feed_count == 1) {  /* Only OpenATV so far, which stays the chosen one. */
		*feed_count = load_feeds(feeds, distro);
		*current = default_feed_index(feeds, *feed_count);
	}
	return choose_feed(ui, input, stop, feeds, *feed_count, *current);
}

void flash_image(struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
{
	struct feed feeds[MAX_FEEDS];
	struct listing l = {0};
	struct target t;
	char distro[32];
	char media[64];
	char footer[160];
	int feed_count = 0;
	int current = 0;
	int selected = 0;
	int generation;
	if (access(OFGWRITE, X_OK) != 0) {
		message(ui, input, stop, _("ofgwrite is not installed, an image cannot be flashed."));
		return;
	}
	if (!find_media(media, sizeof(media))) {
		message(ui, input, stop, _("Neither /media/hdd nor /media/usb is mounted, the image needs one of them. "
			"Please attach a USB stick or a hard disk."));
		return;
	}
	l.images = calloc(MAX_IMAGES + 1, sizeof(*l.images));
	if (!l.images)
		return;
	ui_progress(ui, TITLE, _("Looking for the running slot."), 5, _("Please wait..."), _("Please wait..."));
	memset(&t, 0, sizeof(t));
	boxinfo_value("model", t.model, sizeof(t.model));
	boxinfo_value("mtdkernel", t.mtdkernel, sizeof(t.mtdkernel));
	boxinfo_value("mtdrootfs", t.mtdrootfs, sizeof(t.mtdrootfs));
	find_slot(&t);
	ofgwrite_args(&t);
	boxinfo_value("distro", distro, sizeof(distro));
	feed_count = start_feeds(ui, feeds, distro, &current);
	load_images(ui, &l, &feeds[current]);
	generation = i18n_generation();
	while (!(stop && *stop)) {
		char title[128];
		char body[512];
		char header[96];
		enum input_key key;
		if (generation != i18n_generation()) {  /* The rows in another language. */
			generation = i18n_generation();
			build_rows(&l);
		}
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = ok_label(&l, selected),
			.yellow = _("Distribution"), .back = _("Menu")});
		listing_body(&t, &l, media, body, sizeof(body));
		snprintf(title, sizeof(title), "%s - %s", TITLE, feeds[current].name);
		snprintf(header, sizeof(header), "%s\t%s", _("Image"), _("Size"));
		ui_menu_table(ui, &(struct ui_menu){.title = title, .body = body, .header = header,
			.items = (const char *const *)l.items, .count = l.row_count, .selected = selected, .marks = l.marks,
			.marked = -1, .align = "lr", .footer = footer});
		key = input_next(input, 1000);
		if (key == INPUT_UP || key == INPUT_DOWN || key == INPUT_LEFT || key == INPUT_RIGHT)
			selected = move_selection(&l, key, selected);
		else if (key == INPUT_YELLOW) {
			int chosen = pick_feed(ui, input, stop, feeds, &feed_count, &current, distro);
			if (chosen != current) {
				current = chosen;
				load_images(ui, &l, &feeds[current]);
				selected = 0;
			}
		} else if (key == INPUT_OK && l.rows[selected] <= -2) {  /* The rows before it stay, so does the selection. */
			l.expanded[-2 - l.rows[selected]] ^= 1;
			build_rows(&l);
		} else if (key == INPUT_OK && l.rows[selected] >= 0) {
			prepare(ui, input, stop, &t, &l.images[l.rows[selected]], feeds[current].name, media);
		} else if (key == INPUT_BACK)
			break;
	}
	free_rows(&l);
	free(l.images);
}
