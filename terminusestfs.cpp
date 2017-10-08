/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall fusexmp.c `pkg-config fuse --cflags --libs` -o fusexmp
*/

#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>


#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "plocklib.h"

using namespace std;

const static float SLEEPY_TIME = 0.1;
const static int DELAY_TIME = 60;

static string upper;
static string lower;

static plocklib_simple_t active_commits_lock = PTHREAD_MUTEX_INITIALIZER;
static plocklib_simple_t pending_commits_lock = PTHREAD_MUTEX_INITIALIZER;
static list<pair<string,time_t>> pending_commits;
static list<pair<string,time_t>> pending_luc; //lower-to-upper copies

static plocklib_rw_lock frozen_files_lock = PTHREAD_RWLOCK_INITIALIZER;
static set<string> frozen_files;

static bool two_way;
static volatile bool flush_time = false;

#include <iostream>

static bool exists(string path)
{
     return !faccessat(AT_FDCWD,path.c_str(),F_OK,AT_SYMLINK_NOFOLLOW);
}

static bool special(string path)
{
     struct stat buf;
     lstat(path.c_str(),&buf);
     return ! (S_ISREG(buf.st_mode) || S_ISLNK(buf.st_mode));
}

void* commits_thread(void* ignored)
{
     while(true)
     {
          sleep(5);
          plocklib_become_reader(&frozen_files_lock);
          plocklib_acquire_simple_lock(&pending_commits_lock);
          //cout << "Pending commits: " << pending_commits.size() << endl;
          //for(const auto& x : pending_commits)
          //     cout << x.first << " / " << x.second << endl;
          while(pending_commits.size())
          {
               const auto entry = pending_commits.front();
               if(!frozen_files.count(entry.first) && time(NULL) >= entry.second)
               {
                    pending_commits.pop_front();
                    if(entry.first.find(".fuse_hidden")!=-1 || !exists(upper+"/"+entry.first) || special(upper+"/"+entry.first))
                         continue;
                    plocklib_release_simple_lock(&pending_commits_lock);

                    plocklib_acquire_simple_lock(&active_commits_lock);
                    string rpath = lower+"/"+entry.first.substr(0,entry.first.rfind("/"));
                    auto child_pid = fork();
                    if(!child_pid)
                         execlp("mkdir","mkdir","-p",rpath.c_str(),NULL);
                    else
                         waitpid(child_pid,NULL,0);
                    child_pid = fork();
                    if(!child_pid)
                         execlp("cp","cp","-a",(upper+"/"+entry.first).c_str(),rpath.c_str(),NULL);
                    else
                         waitpid(child_pid,NULL,0);
                    plocklib_release_simple_lock(&active_commits_lock);

                    //We can't just hold the frozen files lock as a reader forever.
                    if(!flush_time)
                    {                         
                         plocklib_resign_as_reader(&frozen_files_lock);
                         sleep(5);
                         plocklib_become_reader(&frozen_files_lock);
                    }
                    plocklib_acquire_simple_lock(&pending_commits_lock);
               }
               else
                    break;
          }
          plocklib_release_simple_lock(&pending_commits_lock);
          plocklib_resign_as_reader(&frozen_files_lock);
     }
}

void* luc_thread(void* ignored)
{
     while(true)
     {
          sleep(5);
          plocklib_become_reader(&frozen_files_lock);
          plocklib_acquire_simple_lock(&pending_commits_lock);
          bool reader = true;
          while(pending_luc.size())
          {
               const auto entry = pending_luc.front();
               if(!frozen_files.count(entry.first) && time(NULL) >= entry.second)
               {
                    string rpath = upper+"/"+entry.first.substr(0,entry.first.rfind("/"));
                    while(!plocklib_request_writer_promotion(&frozen_files_lock))
                         plocklib_become_reader(&frozen_files_lock);
                    frozen_files.insert(entry.first.substr(0,entry.first.rfind("/")));
                    frozen_files.insert(entry.first);
                    pending_luc.pop_front();
                    plocklib_release_simple_lock(&pending_commits_lock);
                    plocklib_resign_as_writer(&frozen_files_lock);
                    auto child_pid = fork();
                    if(!child_pid)
                         execlp("mkdir","mkdir","-p",rpath.c_str(),NULL);
                    else
                         waitpid(child_pid,NULL,0);
                    child_pid = fork();
                    if(!child_pid)
                         execlp("cp","cp","-a",(lower+"/"+entry.first).c_str(),rpath.c_str(),NULL);
                    else
                         waitpid(child_pid,NULL,0);
                    plocklib_become_reader(&frozen_files_lock);
                    while(!plocklib_request_writer_promotion(&frozen_files_lock))
                         plocklib_become_reader(&frozen_files_lock);

                    plocklib_acquire_simple_lock(&pending_commits_lock);
                    frozen_files.erase(entry.first);
                    frozen_files.erase(entry.first.substr(0,entry.first.rfind("/")));
                    plocklib_resign_as_writer(&frozen_files_lock);
                    reader = false;
               }
               else
                    break;
          }
          plocklib_release_simple_lock(&pending_commits_lock);
          if(reader)
               plocklib_resign_as_reader(&frozen_files_lock);
     }
}

//wait until unfrozen then keep lock
static void wuutkl(function<bool()>& predicate)
{
     //Check if file is frozen
     plocklib_become_reader(&frozen_files_lock);
     bool is_frozen = false;
     is_frozen = predicate();
     while(is_frozen)
     {
          plocklib_resign_as_reader(&frozen_files_lock);
          usleep((int)(SLEEPY_TIME*1000000));
          plocklib_become_reader(&frozen_files_lock);
          is_frozen = predicate();
     }
}

//wait until unfrozen then keep lock
static void wuutkl(const char* path)
{
     function<bool()> pred = [&]() { return frozen_files.count(path); };
     wuutkl(pred);
}

//wait until unfrozen then keep lock
static void wuutkl(initializer_list<const char*> paths)
{
     function<bool()> pred = [&]()
	    {
		 for(const auto& x : paths)
		      if(frozen_files.count(x))
			   return true;
		 return false;
	    };
     wuutkl(pred);
}

static string handle_read(const char* path)
{
     wuutkl(path);
     
     if(two_way)
     {
          bool lower_already_checked = false;
          if(exists(upper+"/"+path))
          {
               struct stat buffer;
               time_t utime, ltime;
               
               lstat((upper+"/"+path).c_str(),&buffer);
               utime = buffer.st_mtime;
               if(utime < 0)
                    utime = 0;

               if(exists(lower+"/"+path))
               {
                    lstat((lower+"/"+path).c_str(),&buffer);
                    ltime = buffer.st_mtime;
                    if(ltime < 0)
                         ltime = 0;
                    lower_already_checked = true;
               }
               else
                    ltime = 0;

               if(utime >= ltime)
                    return upper+"/"+path;

               //Otherwise, both exist but lower is newer
               //Delete upper file and quash any pending commits
               plocklib_acquire_simple_lock(&pending_commits_lock);
               unlink((upper+"/"+path).c_str());
               remove_if(pending_commits.begin(),pending_commits.end(),
                         [&](pair<string,time_t> x)
                         {
                              return x.first==path;
                         });
               plocklib_release_simple_lock(&pending_commits_lock);
          }
          if(lower_already_checked || exists(lower+"/"+path))
          {
               plocklib_acquire_simple_lock(&pending_commits_lock);
               if(find_if(pending_luc.begin(),pending_luc.end(),
                          [&](pair<string,time_t> x)
                          {
                               return x.first==path;
                          })==pending_luc.end())
                    pending_luc.emplace_back(path,time(NULL)+DELAY_TIME);
               plocklib_release_simple_lock(&pending_commits_lock);
               return lower+"/"+path;
          }
     }
     else
          if(exists(upper+"/"+path))
               return upper+"/"+path;
          else if(exists(lower+"/"+path))
               return lower+"/"+path;
     
     return upper+"/"+path;
}

static string handle_write(const char* path)
{
     wuutkl(path);
     
     if(two_way)
          handle_read(path);

     auto add_pending_commit = [&]()
     {
          plocklib_acquire_simple_lock(&pending_commits_lock);
          pending_commits.remove_if([&](pair<string,time_t> x)
          {
               return x.first==path;
          });
          pending_luc.remove_if([&](pair<string,time_t> x)
          {
               return x.first==path;
          });
          pending_commits.emplace_back(path,time(NULL)+DELAY_TIME);
          plocklib_release_simple_lock(&pending_commits_lock);
     };

     if(exists(upper+"/"+path) && !special(upper+"/"+path))
     {
          add_pending_commit();
          return upper+"/"+path;
     }
     else if(exists(upper+"/"+path))
          return upper+"/"+path;
     
     string rpath = upper+"/"+string{path}.substr(0,string{path}.rfind("/"));
     string lpath = lower+"/"+string{path}.substr(0,string{path}.rfind("/"));
     if(exists(lpath))
     {
          while(!plocklib_request_writer_promotion(&frozen_files_lock))
               plocklib_become_reader(&frozen_files_lock);
          frozen_files.insert(string{path}.substr(0,string{path}.rfind("/")));
          if(exists(lower+"/"+path))
               frozen_files.insert(path);
          plocklib_resign_as_writer(&frozen_files_lock);

          auto child_pid = fork();
          if(!child_pid)
               execlp("mkdir","mkdir","-p",rpath.c_str(),NULL);
          else
               waitpid(child_pid,NULL,0);
          if(exists(lower+"/"+path))
          {
               child_pid = fork();
               if(!child_pid)
                    execlp("cp","cp","-a",(lower+"/"+path).c_str(),rpath.c_str(),NULL);
               else
                    waitpid(child_pid,NULL,0);
          }

          plocklib_become_reader(&frozen_files_lock);
          while(!plocklib_request_writer_promotion(&frozen_files_lock))
               plocklib_become_reader(&frozen_files_lock);
          frozen_files.erase(string{path}.substr(0,string{path}.rfind("/")));
          if(exists(lower+"/"+path))
               frozen_files.erase(path);
          plocklib_resign_as_writer(&frozen_files_lock);

          wuutkl(path);
     }
     else
          return upper+"/"+path;
     
     add_pending_commit();
     return upper+"/"+path;
}

static int tefs_getattr(const char *path, struct stat *stbuf)
{
     string fname = handle_read(path);
     
     int res;
     res = lstat(fname.c_str(), stbuf);
     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;
     
     return 0;
}

static int tefs_access(const char *path, int mask)
{
     string fname = handle_read(path);
     
     int res;
     res = access(fname.c_str(), mask);
     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;

     return 0;
}

static int tefs_readlink(const char *path, char *buf, size_t size)
{
     string fname = handle_read(path);
     
     int res;
     res = readlink(fname.c_str(), buf, size - 1);
     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;

     buf[res] = '\0';
     return 0;
}

static int tefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
     string fname = handle_read(path);
     map<string,struct stat> file_map;
     
     DIR *dp;
     struct dirent *de;

     (void) offset;
     (void) fi;

     dp = opendir(fname.c_str());
     if (dp == NULL)
     {
          plocklib_resign_as_reader(&frozen_files_lock);
          return -errno;
     }

     while ((de = readdir(dp)) != NULL)
     {
          struct stat st;
          memset(&st, 0, sizeof(st));
          st.st_ino = de->d_ino;
          st.st_mode = de->d_type << 12;
          file_map.insert(pair<string,struct stat>(de->d_name,st));
     }
     closedir(dp);
     
     if(fname.find(upper)==0 && (dp = opendir((lower+"/"+path).c_str()))!=NULL)
     {
          while ((de = readdir(dp)) != NULL)
          {
               struct stat st;
               memset(&st, 0, sizeof(st));
               st.st_ino = de->d_ino;
               st.st_mode = de->d_type << 12;
               file_map.insert(pair<string,struct stat>(de->d_name,st));
          }
          closedir(dp);
     }

     for(const auto& entry : file_map)
          if(filler(buf, entry.first.c_str(), &entry.second, 0))
               break;
     
     plocklib_resign_as_reader(&frozen_files_lock);
     return 0;
}

static int tefs_mknod(const char *path, mode_t mode, dev_t rdev)
{
     mode |= S_IRUSR | S_IWUSR;
     
     string fname;
     if(S_ISREG(mode))
          fname = handle_write(path);
     else
     {
          wuutkl(path);
          fname = upper+"/"+path;
     }
     
     int res;

     /* On Linux this could just be 'mknod(path, mode, rdev)' but this
        is more portable */
     if (S_ISREG(mode)) {
          res = open(fname.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode);
          if (res >= 0)
               res = close(res);
     } else if (S_ISFIFO(mode))
          res = mkfifo(fname.c_str(), mode);
     else
          res = mknod(fname.c_str(), mode, rdev);

     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;

     return 0;
}

static int tefs_mkdir(const char *path, mode_t mode)
{
     mode |= S_IRUSR | S_IWUSR;

     string fname = handle_write(path);
     int res;

     res = mkdir(fname.c_str(), mode);
     mkdir((lower+"/"+path).c_str(),mode);

     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;

     return 0;
}

static int tefs_unlink(const char *path)
{
     wuutkl(path);
     plocklib_acquire_simple_lock(&pending_commits_lock);
     remove_if(pending_commits.begin(),pending_commits.end(),
               [&](pair<string,time_t> x)
               {
                    return x.first==path;
               });
     remove_if(pending_luc.begin(),pending_luc.end(),
               [&](pair<string,time_t> x)
               {
                    return x.first==path;
               });
     plocklib_release_simple_lock(&pending_commits_lock);
     
     int res;
     res = unlink((lower+"/"+path).c_str());
     res = unlink((upper+"/"+path).c_str())==-1 ? res : 0;

     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;

     return 0;
}

static int tefs_rmdir(const char *path)
{
     wuutkl(path);
     plocklib_acquire_simple_lock(&pending_commits_lock);
     remove_if(pending_commits.begin(),pending_commits.end(),
               [&](pair<string,time_t> x)
               {
                    return x.first==path;
               });
     remove_if(pending_luc.begin(),pending_luc.end(),
               [&](pair<string,time_t> x)
               {
                    return x.first==path;
               });
     plocklib_release_simple_lock(&pending_commits_lock);

     int res;
     res = rmdir((lower+"/"+path).c_str());
     res = rmdir((upper+"/"+path).c_str())==-1 ? res : 0;
     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;

     return 0;
}

static int tefs_symlink(const char *from, const char *to)
{
     string fname = handle_write(to);
     int res;

     res = symlink(from, fname.c_str());
     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;
     
     plocklib_acquire_simple_lock(&pending_commits_lock);
     pending_commits.emplace_back(to,time(NULL)+DELAY_TIME);
     plocklib_release_simple_lock(&pending_commits_lock);
     return 0;
}

static int tefs_rename(const char *from, const char *to)
{
     string fdir = string{from}+"/";
     string tdir = string{to}+"/";
     
     string from_name = handle_write(from);
     struct stat buf;
     lstat(from_name.c_str(),&buf);
     plocklib_resign_as_reader(&frozen_files_lock);

     string to_name = handle_write(to);
     plocklib_resign_as_reader(&frozen_files_lock);

     plocklib_acquire_simple_lock(&active_commits_lock);
     
     if(S_ISDIR(buf.st_mode))
     {
          function<bool()> subpath_pred = [&]()
               {
                    for(auto x : frozen_files)
                         if(!x.find(fdir))
                              return true;
                    return false;
               };
          wuutkl(subpath_pred);
          
          plocklib_acquire_simple_lock(&pending_commits_lock);
          transform(pending_commits.begin(),pending_commits.end(),pending_commits.begin(),
                    [&](const auto& x)
                    {
                         string newpath = x.first;
                         if(!newpath.find(fdir))
                              newpath.replace(0,fdir.length(),tdir);
                         return make_pair(newpath,x.second);
                    });
          plocklib_release_simple_lock(&pending_commits_lock);

          rename((lower+"/"+from).c_str(),(lower+"/"+to).c_str());
     }
     else
          wuutkl({from,to});

     int res;
     res = rename(from_name.c_str(), to_name.c_str());
     
     plocklib_release_simple_lock(&active_commits_lock);
     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;

     tefs_unlink(from);
     return 0;
}

static int tefs_chmod(const char *path, mode_t mode)
{
     mode |= S_IRUSR | S_IWUSR;
     if(exists(upper+"/"+path))
          chmod((upper+path).c_str(), mode);
     if(exists(lower+"/"+path))
          if(!fork())
          {
               chmod((lower+path).c_str(), mode);
               exit(0);
          }
     
     return 0;
}

static int tefs_chown(const char *path, uid_t uid, gid_t gid)
{
     if(exists(upper+"/"+path))
          lchown((upper+path).c_str(), uid, gid);
     if(exists(lower+"/"+path))
          if(!fork())
          {
               lchown((lower+path).c_str(), uid, gid);
               exit(0);
          }
     
     return 0;
}

static int tefs_truncate(const char *path, off_t size)
{
     string fname = handle_write(path);
     int res;

     res = truncate(fname.c_str(), size);
     plocklib_resign_as_reader(&frozen_files_lock);
     if (res == -1)
          return -errno;

     return 0;
}

static int tefs_utimens(const char *path, const struct timespec ts[2])
{
     /* don't use utime/utimes since they follow symlinks */
     if(exists(upper+"/"+path))
          utimensat(0, (upper+path).c_str(), ts, AT_SYMLINK_NOFOLLOW);
     if(exists(lower+"/"+path))
          if(!fork())
          {
               utimensat(0, (lower+path).c_str(), ts, AT_SYMLINK_NOFOLLOW);
               exit(0);
          }
     
     return 0;
}

static int tefs_open(const char *path, struct fuse_file_info *fi)
{
     if(fi->flags & O_RDONLY)
          handle_read(path);
     else
          handle_write(path);
     plocklib_resign_as_reader(&frozen_files_lock);
     return 0;
}

static int tefs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
     string fname = handle_read(path);
     int fd;
     int res;

     (void) fi;
     fd = open(fname.c_str(), O_RDONLY);
     if (fd == -1)
     {
          plocklib_resign_as_reader(&frozen_files_lock);
          return -errno;
     }

     res = pread(fd, buf, size, offset);
     if (res == -1)
          res = -errno;
     close(fd);
     
     plocklib_resign_as_reader(&frozen_files_lock);
     return res;
}

static int tefs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
     string fname = handle_write(path);
     int fd;
     int res;

     (void) fi;
     fd = open(fname.c_str(), O_WRONLY);
     if (fd == -1)
     {
          plocklib_resign_as_reader(&frozen_files_lock);
          return -errno;
     }

     res = pwrite(fd, buf, size, offset);
     if (res == -1)
          res = -errno;

     close(fd);
     
     plocklib_resign_as_reader(&frozen_files_lock);
     return res;
}

static int tefs_statfs(const char *path, struct statvfs *stbuf)
{
     int res;

     res = statvfs(upper.c_str(), stbuf);
     if (res == -1)
          return -errno;

     return 0;
}

static int tefs_release(const char *path, struct fuse_file_info *fi)
{
     /* Just a stub.	 This method is optional and can safely be left
        unimplemented */

     (void) path;
     (void) fi;
     return 0;
}

static int tefs_fsync(const char *path, int isdatasync,
                      struct fuse_file_info *fi)
{
     /* Just a stub.	 This method is optional and can safely be left
        unimplemented */

     (void) path;
     (void) isdatasync;
     (void) fi;
     return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int tefs_fallocate(const char *path, int mode,
                          off_t offset, off_t length, struct fuse_file_info *fi)
{
     int fd;
     int res;

     (void) fi;

     if (mode)
          return -EOPNOTSUPP;

     fd = open(path, O_WRONLY);
     if (fd == -1)
          return -errno;

     res = -posix_fallocate(fd, offset, length);

     close(fd);
     return res;
}
#endif

static struct fuse_operations tefs_oper = {
	.getattr	= tefs_getattr,
	.access		= tefs_access,
	.readlink	= tefs_readlink,
	.readdir	= tefs_readdir,
	.mknod		= tefs_mknod,
	.mkdir		= tefs_mkdir,
	.symlink	= tefs_symlink,
	.unlink		= tefs_unlink,
	.rmdir		= tefs_rmdir,
	.rename		= tefs_rename,
//	.link		= tefs_link,
	.chmod		= tefs_chmod,
	.chown		= tefs_chown,
	.truncate	= tefs_truncate,
	.utimens	= tefs_utimens,
	.open		= tefs_open,
	.read		= tefs_read,
	.write		= tefs_write,
	.statfs		= tefs_statfs,
	.release	= tefs_release,
	.fsync		= tefs_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= tefs_fallocate,
#endif
};

int main(int argc, char *argv[])
{
     //Pull out upper and lower paths
     char* buf;
     
     buf = realpath(argv[argc-3],NULL);
     upper = buf;
     free(buf);
     
     buf = realpath(argv[argc-2],NULL);
     lower = buf;
     free(buf);

     //Fix command line parameter list
     argv[argc-3] = argv[argc-1];
     argv[argc-2] = NULL;
     argc-=2;

     pthread_t ct, lt;
     pthread_create(&ct,NULL,commits_thread,NULL);
     pthread_create(&lt,NULL,luc_thread,NULL);

     int to_return = fuse_main(argc, argv, &tefs_oper, NULL);

     //Process all pending commits
     flush_time = true;
     plocklib_acquire_simple_lock(&pending_commits_lock);
     while(pending_commits.size())
     {
          plocklib_release_simple_lock(&pending_commits_lock);
          sleep(5);
          plocklib_acquire_simple_lock(&pending_commits_lock);
     }
     plocklib_release_simple_lock(&pending_commits_lock);

     //TODO

     return to_return;
}
