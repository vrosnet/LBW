/* © 2010 David Given.
 * LBW is licensed under the MIT open source license. See the COPYING
 * file in this distribution for the full text.
 */

#include "globals.h"
#include "elfloader/ElfLoader.h"
#include "syscalls/mmap.h"
#include "syscalls/memory.h"
#include "filesystem/FD.h"
#include "filesystem/InterixVFSNode.h"
#include "filesystem/VFS.h"
#include "MemOp.h"
#include <pthread.h>
#include <vector>

using std::vector;

static pthread_mutex_t lock;
static ElfLoader* executable = NULL;
static ElfLoader* interpreter = NULL;
static vector<string> arguments;
static vector<string> environment;

enum
{
	UNKNOWN_EXECUTABLE,
	ELF_EXECUTABLE,
	INTERIX_EXECUTABLE
};

/* Used when a new process is started; initialises everything that needs to
 * be done anew for any process. (Such as pthread mutexes and GS.)
 */
void InitProcess()
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

	int i = pthread_mutex_init(&lock, &attr);
	if (i)
		error("Failed to init process lock: %d!", i);

	pthread_mutexattr_destroy(&attr);
	InitGSStore();
}

/* Used once we've committed to loading a new executable. Deinits all
 * resources that aren't going to be carried over to the new
 * executable. In particular: memory mappings, file descriptors.
 */
void FlushExecutable()
{
	UnmapAll();
	ClearBrk();
	FD::Flush();
}

void Lock()
{
	pthread_mutex_lock(&lock);
}

void Unlock()
{
	pthread_mutex_unlock(&lock);
}

static int probe_executable(const string& filename)
{
	Ref<FD> ref = VFS::OpenFile(filename);
	int fd = ref->GetRealFD();

	char buffer[4];
	if (pread(fd, buffer, sizeof(buffer), 0) == -1)
		return UNKNOWN_EXECUTABLE;

	if (memcmp(buffer, "\x7F" "ELF", 4) == 0)
		return ELF_EXECUTABLE;
	if (memcmp(buffer, "MZ", 2) == 0)
		return INTERIX_EXECUTABLE;
	return UNKNOWN_EXECUTABLE;
}

static void elf_exec(const string& pathname, const char* argv[], const char* environ[])
{
	int argvsize;
	int envsize;

	void* entrypoint;
	ElfLoader* newExecutable = new ElfLoader();
	ElfLoader* newInterpreter = NULL;

	newExecutable->Open(pathname);

	if (newExecutable->HasInterpreter())
	{
		newInterpreter = new ElfLoader();
		newInterpreter->Open(newExecutable->GetInterpreter());
	}

	/* This is the commit point. We're now ready to either load the new
	 * executable, or die trying. As we're about to nuke the process
	 * memory, including the function that called us, we cannot return
	 * from here.
	 */

	try
	{
		/* Before doing anything else, copy the new argument list and
		 * environment into LBW's memory, as we're about to nuke the old
		 * process'.
		 */

		argvsize = 0;
		arguments.clear();
		while (argv[argvsize])
		{
			arguments.push_back(argv[argvsize]);
			argvsize++;
		}

		envsize = 0;
		environment.clear();
		while (environ[envsize])
		{
			environment.push_back(environ[envsize]);
			envsize++;
		}

		/* Now flush the old executable. */

		FlushExecutable();
		if (executable)
		{
			delete executable;
			delete interpreter;
		}
		executable = newExecutable;
		newExecutable = NULL;
		interpreter = newInterpreter;
		newInterpreter = NULL;

		if (interpreter)
		{
			interpreter->Load();
			executable->Load();
			entrypoint = interpreter->GetEntrypoint();
		}
		else
		{
			executable->Load();
			entrypoint = executable->GetEntrypoint();
		}

		InitProcess();

		int auxsize = 7*2 + 1;

		/* Count the environment and argument array, and copy the data out of
		 * the
		 */

		int arraysize = argvsize + 1 + envsize + 1 + auxsize;
		const char* calldata[arraysize];

		int index = 0;
		for (int i = 0; i < argvsize; i++)
			calldata[index++] = arguments[i].c_str();
		calldata[index++] = NULL;
		for (int i = 0; i < envsize; i++)
			calldata[index++] = environment[i].c_str();
		calldata[index++] = NULL;

		calldata[index++] = (const char*) AT_PHDR;
		calldata[index++] = (const char*) &executable->GetProgramHeader(0);
		calldata[index++] = (const char*) AT_PHENT;
		calldata[index++] = (const char*) executable->GetProgramHeaderSize();
		calldata[index++] = (const char*) AT_PHNUM;
		calldata[index++] = (const char*) executable->GetNumProgramHeaders();
		calldata[index++] = (const char*) AT_ENTRY;
		calldata[index++] = (const char*) executable->GetEntrypoint();
		calldata[index++] = (const char*) AT_PAGESZ;
		calldata[index++] = (const char*) 0x1000;
#if 0
		calldata[index++] = (const char*) AT_BASE;
		calldata[index++] = (const char*) (interpreter ? interpreter->GetLoadAddress() : NULL);
		calldata[index++] = (const char*) AT_FLAGS;
		calldata[index++] = (const char*) 0;
#endif
		calldata[index++] = (const char*) AT_NULL;

		asm volatile (
			"mov %1, %%esp; " // set stack to startup data
			"push %0; " // argc
			"push %2; " //routine to call
			"xor %%eax, %%eax; "
			"mov %%eax, %%ebx; "
			"mov %%eax, %%ecx; "
			"mov %%eax, %%edx; "
			"mov %%eax, %%esi; "
			"mov %%eax, %%edi; "
			"mov %%eax, %%ebp; "
			"ret"
			:
			: "r" (argvsize),
			  "r" (calldata),
			  "r" (entrypoint)
			);
		_exit(0);
	}
	catch (int e)
	{
		log("failed to exec() process with errno %d", e);
		_exit(1);
	}
	catch (...)
	{
		log("mysterious exception! terminating");
		_exit(1);
	}
}

static void interix_exec(const string& pathname, const char* argv[], const char* environ[])
{
	/* Change the Interix directory. */

	Ref<VFSNode> node = VFS::GetCWDNode();
	InterixVFSNode* inode = dynamic_cast<InterixVFSNode*>((VFSNode*) node);
	if (inode)
		fchdir(inode->GetRealFD());
	else
		chdir("/");

	/* Adjust the fd map so that Interix sees the same mappings that Linux
	 * is.
	 */

	FD::Flush();
	map<int, int> fdmap = FD::GetFDMap();

	map<int, int>::const_iterator i = fdmap.begin();
	while (i != fdmap.end())
	{
		//log("linuxfd %d maps to realfd %d", i->first, i->second);
		if (i->first != i->second)
		{
			if (fdmap.find(i->second) != fdmap.end())
			{
				int newfd = dup(i->first);
				//log("copying realfd %d -> %d", i->first, newfd);
				fdmap[i->second] = newfd;
			}

			//log("copying realfd %d -> %d", i->second, i->first);
			int fd = dup2(i->second, i->first);
			assert(fd != -1);
		}
		i++;
	}

	execve(pathname.c_str(), (char* const*) argv, (char* const*) environ);
	throw errno;
}

void Exec(const string& pathname, const char* argv[], const char* environ[])
{
	//log("opening %s", argv[0]);

	switch (probe_executable(pathname))
	{
		case ELF_EXECUTABLE:
			return elf_exec(pathname, argv, environ);

		case INTERIX_EXECUTABLE:
			return interix_exec(pathname, argv, environ);
	}

	throw ENOEXEC;
}
