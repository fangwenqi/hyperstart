#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <inttypes.h>

#include "hyper.h"
#include "util.h"
#include "parse.h"
#include "syscall.h"

static int send_exec_finishing(uint64_t seq, int len, int code, int block)
{
	struct hyper_buf *buf = &ctl.tty.wbuf;

	if (buf->get + len > buf->size) {
		uint8_t *data;
		fprintf(stdout, "%s: tty buf full\n", __func__);

		data = realloc(buf->data, buf->size + len);
		if (data == NULL) {
			perror("realloc failed");
			return -1;
		}
		buf->data = data;
		buf->size += len;
	}

	/* no in event, no more data, send eof */
	hyper_set_be64(buf->data + buf->get, seq);
	hyper_set_be32(buf->data + buf->get + 8, len);
	if (len > 12)
		buf->data[buf->get + 12] = code;

	buf->get += len;
	if (!block) {
		hyper_modify_event(ctl.efd, &ctl.tty, EPOLLIN | EPOLLOUT);
		return 0;
	}

	if (hyper_setfd_block(ctl.tty.fd) < 0 ||
	    hyper_send_data(ctl.tty.fd, buf->data, buf->get) < 0 ||
	    hyper_setfd_nonblock(ctl.tty.fd) < 0) {
		fprintf(stderr, "send eof failed\n");
		return -1;
	}

	return 0;
}

static int hyper_send_exec_eof(struct hyper_exec *exec, int block) {
	return send_exec_finishing(exec->seq, 12, -1, block);
}

static int hyper_send_exec_code(struct hyper_exec *exec, int block) {
	return send_exec_finishing(exec->seq, 13, exec->code, block);
}

static void pts_hup(struct hyper_event *de, int efd, struct hyper_exec *exec)
{
	struct hyper_pod *pod = de->ptr;

	fprintf(stdout, "%s, seq %" PRIu64"\n", __func__, exec->seq);

	hyper_event_hup(de, efd);

	hyper_release_exec(exec, pod);
}

static void stdin_hup(struct hyper_event *de, int efd)
{
	struct hyper_exec *exec = container_of(de, struct hyper_exec, stdinev);
	fprintf(stdout, "%s\n", __func__);
	return pts_hup(de, efd, exec);
}

static void stdout_hup(struct hyper_event *de, int efd)
{
	struct hyper_exec *exec = container_of(de, struct hyper_exec, stdoutev);
	fprintf(stdout, "%s\n", __func__);
	return pts_hup(de, efd, exec);
}

static void stderr_hup(struct hyper_event *de, int efd)
{
	struct hyper_exec *exec = container_of(de, struct hyper_exec, stderrev);
	fprintf(stdout, "%s\n", __func__);
	return pts_hup(de, efd, exec);
}

static int pts_loop(struct hyper_event *de, uint64_t seq, int efd, struct hyper_exec *exec)
{
	int size = -1;
	struct hyper_buf *buf = &ctl.tty.wbuf;

	while ((buf->get + 12 < buf->size) && size) {
		size = read(de->fd, buf->data + buf->get + 12, buf->size - buf->get - 12);
		fprintf(stdout, "%s: read %d data\n", __func__, size);
		if (size < 0) {
			if (errno == EINTR)
				continue;

			if (errno != EAGAIN && errno != EIO) {
				perror("fail to read tty fd");
				return -1;
			}

			break;
		}
		if (size == 0) { // eof
			pts_hup(de, efd, exec);
			break;
		}

		hyper_set_be64(buf->data + buf->get, seq);
		hyper_set_be32(buf->data + buf->get + 8, size + 12);
		buf->get += size + 12;
	}

	if (hyper_modify_event(ctl.efd, &ctl.tty, EPOLLIN | EPOLLOUT) < 0) {
		fprintf(stderr, "modify ctl tty event to in & out failed\n");
		return -1;
	}

	return 0;
}

static int write_to_stdin(struct hyper_event *de, int efd)
{
	struct hyper_exec *exec = container_of(de, struct hyper_exec, stdinev);
	fprintf(stdout, "%s, seq %" PRIu64"\n", __func__, exec->seq);

	int ret = hyper_event_write(de, efd);

	if (ret >= 0 && de->wbuf.get == 0 && exec->close_stdin_request)
		pts_hup(de, efd, exec);

	return ret;
}

struct hyper_event_ops in_ops = {
	.hup		= stdin_hup,
	.write		= write_to_stdin,
	.wbuf_size	= 512,
};

static int stdout_loop(struct hyper_event *de, int efd)
{
	struct hyper_exec *exec = container_of(de, struct hyper_exec, stdoutev);
	fprintf(stdout, "%s, seq %" PRIu64"\n", __func__, exec->seq);

	return pts_loop(de, exec->seq, efd, exec);
}

struct hyper_event_ops out_ops = {
	.read		= stdout_loop,
	.hup		= stdout_hup,
	/* don't need read buff, the pts data will store in tty buffer */
	/* don't need write buff, the stdout data is one way */
};

static int stderr_loop(struct hyper_event *de, int efd)
{
	struct hyper_exec *exec = container_of(de, struct hyper_exec, stderrev);
	fprintf(stdout, "%s, seq %" PRIu64"\n", __func__, exec->errseq);

	return pts_loop(de, exec->errseq ? exec->errseq : exec->seq, efd, exec);
}

struct hyper_event_ops err_ops = {
	.read		= stderr_loop,
	.hup		= stderr_hup,
	/* don't need read buff, the stderr data will store in tty buffer */
	/* don't need write buff, the stderr data is one way */
};

static int hyper_setup_exec_notty(struct hyper_exec *e)
{
	if (e->errseq == 0)
		return -1;

	int inpipe[2];
	if (pipe2(inpipe, O_CLOEXEC) < 0) {
		fprintf(stderr, "creating stderr pipe failed\n");
		return -1;
	}
	hyper_setfd_nonblock(inpipe[1]);
	e->stdinev.fd = inpipe[1];
	e->stdinfd = inpipe[0];

	int outpipe[2];
	if (pipe2(outpipe, O_CLOEXEC) < 0) {
		fprintf(stderr, "creating stderr pipe failed\n");
		return -1;
	}
	hyper_setfd_nonblock(outpipe[0]);
	e->stdoutev.fd = outpipe[0];
	e->stdoutfd = outpipe[1];

	int errpipe[2];
	if (pipe2(errpipe, O_CLOEXEC) < 0) {
		fprintf(stderr, "creating stderr pipe failed\n");
		return -1;
	}
	hyper_setfd_nonblock(errpipe[0]);
	e->stderrev.fd = errpipe[0];
	e->stderrfd = errpipe[1];

	return 0;
}

int hyper_setup_exec_tty(struct hyper_exec *e)
{
	int unlock = 0;
	int ptymaster;
	char ptmx[512], path[512];

	if (e->seq == 0) {
		e->ptyfd = open("/dev/null", O_RDWR | O_NOCTTY | O_CLOEXEC);
		goto done;
	}

	if (!e->tty) { // don't use tty for stdio
		return hyper_setup_exec_notty(e);
	}

	if (e->errseq > 0) {
		int errpipe[2];
		if (pipe2(errpipe, O_CLOEXEC) < 0) {
			fprintf(stderr, "creating stderr pipe failed\n");
			return -1;
		}
		hyper_setfd_nonblock(errpipe[0]);
		e->stderrev.fd = errpipe[0];
		e->stderrfd = errpipe[1];
	}

	if (e->id) {
		if (sprintf(path, "/tmp/hyper/%s/devpts/", e->id) < 0) {
			fprintf(stderr, "get ptmx path failed\n");
			return -1;
		}
	} else {
		if (sprintf(path, "/dev/pts/") < 0) {
			fprintf(stderr, "get ptmx path failed\n");
			return -1;
		}
	}

	if (sprintf(ptmx, "%s/ptmx", path) < 0) {
		fprintf(stderr, "get ptmx path failed\n");
		return -1;
	}

	ptymaster = open(ptmx, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (ptymaster < 0) {
		perror("open ptmx device for execcmd failed");
		return -1;
	}

	if (ioctl(ptymaster, TIOCSPTLCK, &unlock) < 0) {
		perror("ioctl unlock ptmx device failed");
		return -1;
	}

	if (ioctl(ptymaster, TIOCGPTN, &e->ptyno) < 0) {
		perror("ioctl get execcmd pty device failed");
		return -1;
	}

	if (sprintf(ptmx, "%s/%d", path, e->ptyno) < 0) {
		fprintf(stderr, "get ptmx path failed\n");
		return -1;
	}

	e->ptyfd = open(ptmx, O_RDWR | O_NOCTTY | O_CLOEXEC);
	fprintf(stdout, "get pty device for exec %s\n", ptmx);

	e->stdinev.fd = ptymaster;
	e->stdoutev.fd = dup(ptymaster);
done:

	e->stdinfd = e->ptyfd;
	e->stdoutfd = e->ptyfd;
	if (e->errseq == 0) {
		e->stderrev.fd = dup(e->stdoutev.fd);
		e->stderrfd = e->ptyfd;
	}
	fprintf(stdout, "%s pts event %p, fd %d %d\n",
		__func__, &e->stdinev, ptymaster, e->ptyfd);
	return 0;
}

int hyper_dup_exec_tty(int to, struct hyper_exec *e)
{
	int ret = -1;

	fprintf(stdout, "%s\n", __func__);
	setsid();

	if (e->tty && (ioctl(e->ptyfd, TIOCSCTTY, NULL) < 0)) {
		perror("ioctl pty device for execcmd failed");
		goto out;
	}

	fflush(stdout);
	hyper_send_type(to, READY);

	if (dup2(e->stdinfd, STDIN_FILENO) < 0) {
		perror("dup tty device to stdin failed");
		goto out;
	}

	if (dup2(e->stdoutfd, STDOUT_FILENO) < 0) {
		perror("dup tty device to stdout failed");
		goto out;
	}

	if (dup2(e->stderrfd, STDERR_FILENO) < 0) {
		perror("dup err pipe to stderr failed");
		goto out;
	}

	ret = 0;
out:
	return ret;
}

int hyper_watch_exec_pty(struct hyper_exec *exec, struct hyper_pod *pod)
{
	fprintf(stdout, "hyper_init_event container pts event %p, ops %p, fd %d\n",
		&exec->stdinev, &in_ops, exec->stdinev.fd);

	if (exec->seq == 0)
		return 0;

	if (hyper_init_event(&exec->stdinev, &in_ops, pod) < 0 ||
	    hyper_add_event(ctl.efd, &exec->stdinev, EPOLLOUT) < 0) {
		fprintf(stderr, "add container stdin event failed\n");
		return -1;
	}
	exec->ref++;

	if (hyper_init_event(&exec->stdoutev, &out_ops, pod) < 0 ||
	    hyper_add_event(ctl.efd, &exec->stdoutev, EPOLLIN) < 0) {
		fprintf(stderr, "add container stdout event failed\n");
		return -1;
	}
	exec->ref++;

	if (hyper_init_event(&exec->stderrev, &err_ops, pod) < 0 ||
	    hyper_add_event(ctl.efd, &exec->stderrev, EPOLLIN) < 0) {
		fprintf(stderr, "add container stderr event failed\n");
		return -1;
	}
	exec->ref++;
	return 0;
}

int hyper_enter_container(struct hyper_pod *pod,
			  struct hyper_exec *exec)
{
	int ipcns, utsns, mntns, ret;
	struct hyper_container *c;
	char path[512];

	ret = ipcns = utsns = mntns = -1;

	c = hyper_find_container(pod, exec->id);
	if (c == NULL) {
		fprintf(stderr, "can not find container %s\n", exec->id);
		return -1;
	}

	sprintf(path, "/proc/%d/ns/uts", pod->init_pid);
	utsns = open(path, O_RDONLY| O_CLOEXEC);
	if (utsns < 0) {
		perror("fail to open utsns of pod init");
		goto out;
	}

	sprintf(path, "/proc/%d/ns/ipc", pod->init_pid);
	ipcns = open(path, O_RDONLY| O_CLOEXEC);
	if (ipcns < 0) {
		perror("fail to open ipcns of pod init");
		goto out;
	}

	mntns = c->ns;
	if (mntns < 0) {
		perror("fail to open mntns of pod init");
		goto out;
	}

	if (setns(utsns, CLONE_NEWUTS) < 0 ||
	    setns(ipcns, CLONE_NEWIPC) <0 ||
	    setns(mntns, CLONE_NEWNS) < 0) {
		perror("fail to enter container ns");
		goto out;
	}

	/* TODO: wait for container finishing setup root */
	chdir("/");

	ret = hyper_setup_env(c->envs, c->envs_num);
out:
	close(ipcns);
	close(utsns);

	return ret;
}

struct hyper_exec_arg {
	struct hyper_pod	*pod;
	struct hyper_exec	*exec;
	int			pipe[2];
};

static int hyper_do_exec_cmd(void *data)
{
	struct hyper_exec_arg *arg = data;
	struct hyper_exec *exec = arg->exec;
	struct hyper_pod *pod = arg->pod;
	int pipe[2] = {-1, -1}, pid;
	int ret = -1;

	if (exec->id) {
		char path[512];
		int pidns;

		sprintf(path, "/proc/%d/ns/pid", pod->init_pid);
		pidns = open(path, O_RDONLY| O_CLOEXEC);
		if (pidns < 0) {
			perror("fail to open pidns of pod init");
			goto out;
		}

		/* enter pidns of pod init, so the children of this process will run in
		 * pidns of pod init, see man 2 setns */
		if (setns(pidns, CLONE_NEWPID) < 0) {
			perror("enter pidns of pod init failed");
			goto out;
		}
		close(pidns);
	}

	if (pipe2(pipe, O_CLOEXEC) < 0) {
		perror("create pipe in exec command failed");
		goto out;
	}

	if (hyper_watch_exec_pty(exec, pod) < 0) {
		fprintf(stderr, "add pts master event failed\n");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		perror("fail to fork");
		goto out;
	} else if (pid > 0) {
		uint32_t type;

		if (hyper_get_type(pipe[0], &type) < 0 || type != READY) {
			fprintf(stderr, "hyper init doesn't get execcmd ready message\n");
			goto out;
		}

		fprintf(stdout, "hyper init get ready message\n");
		exec->pid = pid;
		//TODO combin ref++ and add to list.
		list_add_tail(&exec->list, &pod->exec_head);
		exec->ref++;
		fprintf(stdout, "create exec cmd %s pid %d,ref %d\n", exec->argv[0], pid, exec->ref);
		ret = 0;
		goto out;
	}

	if (exec->id && hyper_enter_container(pod, exec) < 0) {
		fprintf(stderr, "enter container ns failed\n");
		goto exit;
	}

	if (hyper_dup_exec_tty(pipe[1], exec) < 0) {
		fprintf(stderr, "dup pts to exec stdio failed\n");
		goto exit;
	}

	if (execvp(exec->argv[0], exec->argv) < 0) {
		perror("exec failed");
		goto exit;
	}

	ret = 0;
exit:
	close(pipe[0]);
	close(pipe[1]);
	hyper_send_type(pipe[1], ERROR);
	_exit(ret);
out:
	close(pipe[0]);
	close(pipe[1]);
	hyper_send_type(arg->pipe[1], ret ? ERROR : READY);
	_exit(ret);
}

static void hyper_free_exec(struct hyper_exec *exec)
{
	int i;

	free(exec->id);

	for (i = 0; i < exec->argc; i++) {
		//fprintf(stdout, "argv %d %s\n", i, exec->argv[i]);
		free(exec->argv[i]);
	}

	free(exec->argv);
	free(exec);
}

int hyper_exec_cmd(char *json, int length)
{
	struct hyper_exec *exec;
	struct hyper_pod *pod = &global_pod;
	int stacksize = getpagesize() * 4;
	void *stack = NULL;
	struct hyper_exec_arg arg = {
		.pod	= pod,
		.exec	= NULL,
		.pipe	= {-1, -1},
	};
	int pid, ret = -1, status;
	uint32_t type;

	fprintf(stdout, "call hyper_exec_cmd, json %s, len %d\n", json, length);

	exec = hyper_parse_execcmd(json, length);
	if (exec == NULL) {
		fprintf(stderr, "parse exec cmd failed\n");
		goto out;
	}

	if (exec->argv == NULL) {
		fprintf(stderr, "cmd is %p, seq %" PRIu64 ", container %s\n",
			exec->argv, exec->seq, exec->id);
		goto free_exec;
	}

	if (hyper_setup_exec_tty(exec) < 0) {
		fprintf(stderr, "setup exec tty failed\n");
		goto free_exec;
	}

	if (pipe2(arg.pipe, O_CLOEXEC) < 0) {
		perror("create pipe between pod init execcmd failed");
		goto close_tty;
	}

	arg.exec = exec;

	stack = malloc(stacksize);
	if (stack == NULL) {
		perror("fail to allocate stack for container init");
		goto close_tty;
	}

	pid = clone(hyper_do_exec_cmd, stack + stacksize, CLONE_VM| CLONE_FILES| SIGQUIT, &arg);
	fprintf(stdout, "do_exec_cmd pid %d\n", pid);
	if (pid < 0) {
		perror("clone hyper_do_exec_cmd failed");
		goto close_tty;
	}

	if (waitpid(pid, &status, __WCLONE) <= 0) {
		perror("waiting hyper_do_exec_cmd finish failed");
		goto close_tty;
	}

	if (hyper_get_type(arg.pipe[0], &type) < 0 || type != READY) {
		fprintf(stderr, "hyper init doesn't get execcmd ready message\n");
		goto close_tty;
	}

	fprintf(stdout, "%s get ready message %"PRIu32 "\n", __func__, type);
	ret = 0;
out:
	close(arg.pipe[0]);
	close(arg.pipe[1]);
	free(stack);
	return ret;
close_tty:
	close(exec->ptyfd);
	if (exec->stdinfd != exec->ptyfd)
		close(exec->stdinfd);
	if (exec->stdoutfd != exec->ptyfd)
		close(exec->stdoutfd);
	if (exec->stderrfd != exec->ptyfd)
		close(exec->stderrfd);
	close(exec->stdinev.fd);
	close(exec->stdoutev.fd);
	close(exec->stderrev.fd);
free_exec:
	hyper_free_exec(exec);
	goto out;
}

static int hyper_send_pod_finished(struct hyper_pod *pod)
{
	int ret = -1;
	struct hyper_container *c;
	uint8_t *data = NULL, *new;
	int c_num = 0;

	list_for_each_entry(c, &pod->containers, list) {
		c_num++;
		new = realloc(data, c_num * 4);
		if (new == NULL)
			goto out;

		hyper_set_be32(new + ((c_num - 1) * 4), c->exec.code);
		data = new;
	}

	ret = hyper_send_msg_block(ctl.chan.fd, PODFINISHED, c_num * 4, data);
out:
	free(data);
	return ret;
}

int hyper_release_exec(struct hyper_exec *exec,
		       struct hyper_pod *pod)
{
	if (--exec->ref != 0) {
		fprintf(stdout, "still have %d user of exec\n", exec->ref);
		return 0;
	}

	/* exec has no pty or the pty user already exited */
	fprintf(stdout, "last user of exec exit, release\n");

	hyper_reset_event(&exec->stdinev);
	hyper_reset_event(&exec->stdoutev);
	hyper_reset_event(&exec->stderrev);

	list_del_init(&exec->list);

	hyper_send_exec_eof(exec, 0);

	hyper_send_exec_code(exec, 0);

	fprintf(stdout, "%s exit code %" PRIu8"\n", __func__, exec->code);
	if (exec->init) {
		fprintf(stdout, "%s container init exited, type %d, remains %d, policy %d\n",
			__func__, pod->type, pod->remains, pod->policy);

		// TODO send finish of this container and full cleanup
		if (--pod->remains > 0)
			return 0;

		if (pod->type == STOPPOD) {
			/* stop pod manually, hyper doesn't care the pod finished codes */
			hyper_send_msg_block(ctl.chan.fd, ACK, 0, NULL);
		} else if (pod->type == DESTROYPOD) {
			/* shutdown vm manually, hyper doesn't care the pod finished codes */
			hyper_shutdown();
		} else {
			/* send out pod finish message, hyper will decide if restart pod or not */
			hyper_send_pod_finished(pod);
		}

		hyper_cleanup_pod(pod);
		return 0;
	}

	hyper_free_exec(exec);
	return 0;
}

struct hyper_exec *hyper_find_exec_by_pid(struct list_head *head, int pid)
{
	struct hyper_exec *exec;

	list_for_each_entry(exec, head, list) {
		fprintf(stdout, "exec pid %d, pid %d\n", exec->pid, pid);
		if (exec->pid != pid)
			continue;

		return exec;
	}

	return NULL;
}

struct hyper_exec *hyper_find_exec_by_seq(struct hyper_pod *pod, uint64_t seq)
{
	struct hyper_exec *exec;

	list_for_each_entry(exec, &pod->exec_head, list) {
		fprintf(stdout, "exec seq %" PRIu64 ", seq %" PRIu64 "\n",
			exec->seq, seq);
		if (exec->seq != seq)
			continue;

		return exec;
	}

	return NULL;
}

int hyper_handle_exec_exit(struct hyper_pod *pod, int pid, uint8_t code)
{
	struct hyper_exec *exec;

	exec = hyper_find_exec_by_pid(&pod->exec_head, pid);
	if (exec == NULL) {
		fprintf(stdout, "can not find exec whose pid is %d\n",
			pid);
		return 0;
	}

	fprintf(stdout, "%s exec exit pid %d, seq %" PRIu64 ", container %s\n",
		__func__, exec->pid, exec->seq, exec->id ? exec->id : "pod");

	exec->code = code;
	exec->exit = 1;

	close(exec->ptyfd);
	exec->ptyfd = -1;
	if (exec->stdinfd != exec->ptyfd)
		close(exec->stdinfd);
	exec->stdinfd = -1;
	if (exec->stdoutfd != exec->ptyfd)
		close(exec->stdoutfd);
	exec->stdoutfd = -1;
	if (exec->stderrfd != exec->ptyfd)
		close(exec->stderrfd);
	exec->stderrfd = -1;

	hyper_release_exec(exec, pod);

	return 0;
}

void hyper_cleanup_exec(struct hyper_pod *pod)
{
	struct hyper_exec *exec, *next;

	list_for_each_entry_safe(exec, next, &pod->exec_head, list) {
		fprintf(stdout, "send eof for exec seq %" PRIu64 "\n", exec->seq);
		if (hyper_send_exec_eof(exec, 1) < 0 ||
		    hyper_send_exec_code(exec, 1) < 0)
			fprintf(stderr, "send eof failed\n");
	}
}
