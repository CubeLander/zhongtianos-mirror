#include <fs/buf.h>
#include <fs/cluster.h>
#include <fs/fat32.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/error.h>
#include <lib/log.h>
#include <lib/queue.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <lock/mutex.h>
#include <proc/cpu.h>
#include <proc/thread.h>
#include <sys/errno.h>
#include <fs/filepnt.h>

u64 used_dirents = 0;

/**
 * 本文件用于维护Dirent的分配以及维护和修改Dirent的树状结构
 */

extern FileSystem *fatFs;
extern mutex_t mtx_file;

// 待分配的dirent
Dirent *dirents;
struct DirentList dirent_free_list = {NULL};

#define PEEK sizeof(dirents)

// 管理dirent分配和释放的互斥锁
struct mutex mtx_dirent;

void dirent_init() {
	mtx_init(&mtx_dirent, "Dirent", 1, MTX_SPIN);

	for (int i = 0; i < MAX_DIRENT; i++) {
		LIST_INSERT_HEAD(&dirent_free_list, &dirents[i], dirent_link);
	}

	log(LEVEL_GLOBAL, "dirent Init Finished!\n");
}

// Dirent的分配和释放属于对Dirent整体的操作，需要加锁
Dirent *dirent_alloc() {
	mtx_lock(&mtx_dirent);

	panic_on(LIST_EMPTY(&dirent_free_list));
	Dirent *dirent = LIST_FIRST(&dirent_free_list);
	// TODO: 需要初始化dirent的睡眠锁
	LIST_REMOVE(dirent, dirent_link);
	memset(dirent, 0, sizeof(Dirent));
	used_dirents += 1;
	dirent->mode = 0777;
	mtx_unlock(&mtx_dirent);
	return dirent;
}

void dirent_dealloc(Dirent *dirent) {
	mtx_lock(&mtx_dirent);

	memset(dirent, 0, sizeof(Dirent));
	LIST_INSERT_HEAD(&dirent_free_list, dirent, dirent_link);
	used_dirents -= 1;

	mtx_unlock(&mtx_dirent);
}

/**
 * @brief 跳过左斜线。unix传统，允许路径上有连续的多个左斜线，解析时看作一条
 */
static char *skip_slash(char *p) {
	while (*p == '/') {
		p++;
	}
	return p;
}

/**
 * @brief 将dirent的引用计数加一
 */
void dget(Dirent *dirent) {
	int is_filled = 0;
	int i;
	int cur_proc_index = get_proc_index(cpu_this()->cpu_running->td_proc);
	for (i = 0; i < dirent->holder_cnt + 1; i++) {
		if (dirent->holders[i].proc_index == cur_proc_index) {
			dirent->holders[i].cnt += 1;
			is_filled = 1;
			break;
		} else if (dirent->holders[i].proc_index == 0) {
			dirent->holders[i].proc_index = cur_proc_index;
			dirent->holders[i].cnt = 1;

			#ifdef REFCNT_DEBUG
			// strncpy(dirent->holders[i].proc_name, cpu_this()->cpu_running->td_name, MAX_NAME_LEN);
			dirent->holders[i].proc_name = cpu_this()->cpu_running->td_name;
			#endif

			dirent->holder_cnt += 1;
			is_filled = 1;
			break;
		}
	}
	if (!is_filled) {
		for (int i = 0; i < dirent->holder_cnt; i++) {
			#ifdef REFCNT_DEBUG
			printf("dget: %s, process: %s, holder: %s, hold_cnts\n", dirent->name,
				cpu_this()->cpu_running->td_name,
				dirent->holders[i].proc_name,
				dirent->holders[i].cnt);
			#else
			printf("dget: %s, process: %s, holder: %d, hold_cnts\n", dirent->name,
				cpu_this()->cpu_running->td_name,
				dirent->holders[i].proc_index,
				dirent->holders[i].cnt);
			#endif
		}
		panic("dget: file %s holder_cnt is full!\n", dirent->name);
	}

#ifdef REFCNT_DEBUG
	mtx_lock_sleep(&mtx_file);
	warn("dget: %s, process: %s, cur_hold_cnt: %d\n", dirent->name, cpu_this()->cpu_running->td_name, dirent->holders[i].cnt);
	mtx_unlock_sleep(&mtx_file);
#endif
	dirent->refcnt += 1;
}

/**
 * @brief 将dirent的引用数减一
 */
void dput(Dirent *dirent) {
	u16 index = get_proc_index(cpu_this()->cpu_running->td_proc);
	#ifdef REFCNT_DEBUG
	u16 rmed = 0;
	#endif
	int i;
	for (i = 0; i < dirent->holder_cnt; i++) {
		if (dirent->holders[i].proc_index == index) {
			dirent->holders[i].cnt -= 1;
			if (dirent->holders[i].cnt == 0) {
				dirent->holders[i] = dirent->holders[dirent->holder_cnt - 1];
				dirent->holders[dirent->holder_cnt - 1] = (struct holder_info){0, 0};
				dirent->holder_cnt -= 1;
				#ifdef REFCNT_DEBUG
				rmed = 1;
				#endif
			}
			break;
		}
	}

#ifdef REFCNT_DEBUG
	mtx_lock_sleep(&mtx_file);
	warn("dput: %s, process: %s, cur_hold_cnt: %d\n", dirent->name, cpu_this()->cpu_running->td_name, rmed == 1 ? 0 : dirent->holders[i].cnt);
	mtx_unlock_sleep(&mtx_file);
#endif
	dirent->refcnt -= 1;
}

/**
 * @brief 获取某个Dirent的父级Dirent，需要考虑mount的情形
 */
static Dirent *get_parent_dirent(Dirent *dirent) {

	FileSystem *fs = dirent->file_system;
	Dirent *ans = NULL;

	if (dirent == fs->root) {
		// 本级就是文件系统的根目录
		// 跨级问题当且仅当当前处于文件系统根目录
		if (fs->mountPoint != NULL) {
			ans = get_parent_dirent(fs->mountPoint);
		} else {
			warn("reach root directory. it\'s parent dirent is self!\n");
			ans = dirent;
		}
	} else {
		// 包括上一级是根目录的情况，默认只导向到根目录，不导向挂载点
		ans = dirent->parent_dirent;
	}

	return ans;
}

/**
 * @brief 在dir中找一个名字为name的文件，找到后获取其引用
 * @note 只需要遍历Dirent树即可，无需实际访问磁盘
 */
static int dir_lookup(FileSystem *fs, Dirent *dir, char *name, struct Dirent **file) {
	Dirent *child;

	LIST_FOREACH (child, &dir->child_list, dirent_link) {

		if (strncmp(name, child->name, MAX_NAME_LEN) == 0) {
			dget(child);

			*file = child;
			return 0;
		}
	}

	warn("dir_lookup: %s not found in %s\n", name, dir->name);
	return -ENOENT;
}

/**
 * @brief 尝试将dirent从当前的mountPoint转移到mountFs内部的root目录项
 */
static Dirent *try_enter_mount_dir(Dirent *dir) {
	// Note: 处理mount的目录

	if (IS_MOUNT_DIR(dir)) {
		FileSystem *fs = find_fs_by(find_fs_of_dir, dir);
		if (fs == NULL) {
			warn("load mount fs error on dir %s!\n", dir->name);

			panic("");
		}
		dir = fs->root;

		return dir;
	}

	return dir;
}

/**
 * @brief 顺序获取路径上（包括自己）的所有目录引用（可以理解为意向锁）
 * @note 保证路径上的引用一定全不为0，因此无需顺序获取
 */
void dget_path(Dirent *file) {
	if (file == NULL) {
		panic("dget_path: file is NULL!\n");
	}
	while (1) {
		dget(file);
		if (file == file->file_system->root) {
			file = file->file_system->mountPoint;
			if (file == NULL)
				return;
		} else {
			file = get_parent_dirent(file);
		}
	}
}

/**
 * @brief 逆序释放路径上（包括自己）的所有目录引用（可以理解为意向锁）
 */
void dput_path(Dirent *file) {
	while (1) {
		dput(file);
		if (file == file->file_system->root) {
			file = file->file_system->mountPoint;
			if (file == NULL)
				return;
		} else {
			file = get_parent_dirent(file);
		}
	}
}

/**
 * @brief 遍历路径，找到某个文件。若找到，则获取该文件的引用
 * @todo 动态根据Dirent识别 fs，以及引入链接和虚拟文件的机制
 * @param baseDir 开始遍历的根，为 NULL 表示忽略。但如果path以'/'开头，则强制使用根目录
 * @param pdir 文件所在的目录
 * @param pfile 文件本身
 * @param lastelem 如果恰好找到了文件的上一级目录的位置，则返回最后未匹配的那个项目的名称(legacy)
 */
int walk_path(FileSystem *fs, char *path, Dirent *baseDir, Dirent **pdir, Dirent **pfile,
	      char *lastelem, longEntSet *longSet) {
	char *p;
	char name[MAX_NAME_LEN];
	Dirent *dir, *file, *tmp;
	int r;

	// 计算初始Dirent
	if (path[0] == '/' || baseDir == NULL) {
		file = fs->root;
	} else {
		// 持有baseDir这个Dirent的进程同样持有自己上级所有目录的意向锁
		file = baseDir;
	}

	dget_path(file);

	path = skip_slash(path);
	dir = NULL;
	name[0] = 0;
	*pfile = 0;

	while (*path != '\0') {
		dir = file;
		p = path;

		// 1. 循环读取文件名
		while (*path != '/' && *path != '\0') {
			path++;
		}

		if (path - p >= MAX_NAME_LEN) {
			return -ENAMETOOLONG;
		}

		memcpy(name, p, path - p);
		name[path - p] = '\0';

		path = skip_slash(path);

		// 2. 检查目录的属性
		// 如果不是目录，则直接报错
		if (!IS_DIRECTORY(&dir->raw_dirent)) {
			// 中途扫过的一个路径不是目录
			// 放掉中途所有引用
			dput_path(dir);
			return -ENOTDIR;
		}

		tmp = try_enter_mount_dir(dir);
		if (tmp != dir) {
			dget(tmp);
			dir = tmp;
		}

		// 处理 "." 和 ".."
		if (strncmp(name, ".", 2) == 0) {
			// 维持dir和file不变
			continue;
		} else if (strncmp(name, "..", 3) == 0) {
			// 回溯需要放引用
			Dirent *old_dir = dir;

			// 先获取上一级目录，再放引用，防止后续引用无效
			dir = get_parent_dirent(dir);
			file = get_parent_dirent(file);

			if (dir != old_dir) { // 排除dir是根目录的情况（根目录的上一级目录是自己）
				dput(old_dir);
			}

			// 挂载的目录
			if (old_dir == old_dir->file_system->root && old_dir->file_system->mountPoint) {
				dput(old_dir->file_system->mountPoint);
			}
			continue;
		}

		// 3. 继续遍历目录
		if ((r = dir_lookup(fs, dir, name, &file)) < 0) {
			// printf("r = %d\n", r);
			// *path == '\0'表示遍历到最后一个项目了
			if (r == -ENOENT && *path == '\0') {
				if (pdir) {
					*pdir = dir;
				}

				if (lastelem) {
					strncpy(lastelem, name, MAX_NAME_LEN);
				}

				*pfile = 0;
			}

			// 失败时，回溯意向锁
			dput_path(dir);
			return r;
		}
	}

	tmp = try_enter_mount_dir(file);
	if (tmp != file) {
		dget(tmp);
		file = tmp;
	}

	if (pdir) {
		*pdir = dir;
	}
	*pfile = file;
	return 0;
}

/**
 * @brief 传入一个Dirent，获取其绝对路径
 */
void dirent_get_path(Dirent *dirent, char *path) {
	assert(dirent->refcnt > 0);

	Dirent *tmp = dirent;
	path[0] = 0; // 先把path清空为空串

	// 是根目录
	if (tmp == fatFs->root) {
		strins(path, "/");
	}

	while (tmp != fatFs->root) {
		if (tmp == tmp->file_system->root) {
			FileSystem *fs = tmp->file_system;
			tmp = fs->mountPoint;
		}

		strins(path, tmp->name);
		strins(path, "/");
		tmp = get_parent_dirent(tmp);
	}
}

static int createItemAt(struct Dirent *baseDir, char *path, Dirent **file, int isDir) {
	mtx_lock_sleep(&mtx_file);

	char lastElem[MAX_NAME_LEN];
	Dirent *dir = NULL, *f = NULL;
	int r;
	longEntSet longSet;
	FileSystem *fs;
	extern FileSystem *fatFs;

	if (baseDir) {
		fs = baseDir->file_system;
	} else {
		fs = fatFs;
	}

	// 1. 寻找要创建的文件
	if ((r = walk_path(fs, path, baseDir, &dir, &f, lastElem, &longSet)) == 0) {
		file_close(f);
		warn("file or directory exists: %s\n", path);

		mtx_unlock_sleep(&mtx_file);
		return -EEXIST;
	}

	// 2. 处理错误：当出现其他错误，或者没有找到上一级的目录时，退出
	if (r != -ENOENT || dir == NULL) {
		mtx_unlock_sleep(&mtx_file);
		return r;
	}

	// 获取新创建的文件父级目录及之前的引用
	// todo: 完全的并发环境下这里会产生引用空洞（导致dir可能被删除），需要在walk_path中就保留dir的引用！
	dget_path(dir);

	// 3. 分配Dirent，并获取新创建文件的引用
	if ((r = dir_alloc_file(dir, &f, lastElem)) < 0) {
		mtx_unlock_sleep(&mtx_file);
		return r;
	}

	// 4. 填写Dirent的各项信息
	f->parent_dirent = dir;				   // 设置父亲节点，以安排写回
	f->file_system = dir->file_system;
	extern struct FileDev file_dev_file;
	f->dev = &file_dev_file; // 赋值设备指针
	f->type = (isDir) ? DIRENT_DIR : DIRENT_FILE;

	// 5. 目录应当以其分配了的大小为其文件大小（TODO：但写回时只写回0）
	if (isDir) {
		// 目录至少分配一个簇
		int clusSize = CLUS_SIZE(dir->file_system);
		f->first_clus = clusterAlloc(dir->file_system, 0); // 在Alloc时即将first_clus清空为全0
		f->file_size = clusSize;
		f->raw_dirent.DIR_Attr = ATTR_DIRECTORY;
	} else {
		// 空文件不分配簇
		f->file_size = 0;
		f->first_clus = 0;
	}
	filepnt_init(f);

	// 4. 将dirent加入到上级目录的子Dirent列表
	LIST_INSERT_HEAD(&(dir->child_list), f, dirent_link);

	// 5. 回写dirent信息
	sync_dirent_rawdata_back(f);

	if (file) {
		*file = f;
	}

	mtx_unlock_sleep(&mtx_file);
	return 0;
}

/**
 * @brief 在dir目录下新建一个名为path的目录。忽略mode参数
 * @return 0成功，-1失败
 */
int makeDirAt(Dirent *baseDir, char *path, int mode) {
	Dirent *dir = NULL;
	int ret = createItemAt(baseDir, path, &dir, 1);
	if (ret < 0) {
		return ret;
	} else {
		file_close(dir);
		return 0;
	}
}

/**
 * @brief 创建一个文件
 */
int r;
int createFile(struct Dirent *baseDir, char *path, Dirent **file) {
	return createItemAt(baseDir, path, file, 0);
}

int create_file_and_close(char *path) {
	Dirent *file = NULL;
	r = createFile(NULL, path, &file);
	if (r < 0) {
		warn("create file error!\n");
		return r;
	}
	file_close(file);
	return 0;
}
