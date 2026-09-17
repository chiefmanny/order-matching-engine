"""Launching, supervising and stopping the processes that make up an experiment.

Two backends, because the binaries need gRPC:

  * `local`  - run ./build/<binary> directly. Needs gRPC installed on the host.
  * `docker` - run the engine as a container and exec the rest into it. This is the
               normal path: the repo's toolchain is containerised, and a dev machine
               without gRPC can still run full experiments.

Either way the harness only ever *supervises* processes. It never simulates market
behavior: the engine and every participant are the real C++ binaries.
"""
from __future__ import annotations

import shlex
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import IO, Callable


class LaunchError(RuntimeError):
    pass


@dataclass
class ManagedProcess:
    name: str
    popen: subprocess.Popen
    stdout_path: Path
    stderr_path: Path
    started_at: float
    _stdout: IO | None = None
    _stderr: IO | None = None
    stop_requested_at: float | None = None
    killed: bool = False
    # Sends a signal to the *real* process. Under docker the popen handle is only the
    # client end of `docker exec`, which does not forward signals into the container.
    signaller: Callable[[int], None] | None = None

    def poll(self) -> int | None:
        return self.popen.poll()

    def send(self, sig: int) -> None:
        if self.signaller is not None:
            self.signaller(sig)
            return
        try:
            self.popen.send_signal(sig)
        except ProcessLookupError:
            pass

    def close_files(self) -> None:
        for handle in (self._stdout, self._stderr):
            if handle and not handle.closed:
                handle.flush()
                handle.close()


class Launcher:
    """Base launcher: runs binaries directly from a build directory."""

    def __init__(self, bin_dir: Path):
        self.bin_dir = Path(bin_dir)

    def command(self, name: str, args: list[str]) -> list[str]:
        path = self.bin_dir / name
        if not path.exists():
            raise LaunchError(
                f"binary not found: {path}\n"
                f"Build it first, or use --backend docker if gRPC is not installed here."
            )
        return [str(path), *args]

    def container_path(self, host_path: Path) -> str:
        return str(host_path)

    def run_sync(self, name: str, args: list[str], timeout: float = 10.0):
        return subprocess.run(self.command(name, args), capture_output=True,
                              text=True, check=False, timeout=timeout)

    def start(self, name: str, binary: str, args: list[str], log_dir: Path) -> ManagedProcess:
        log_dir.mkdir(parents=True, exist_ok=True)
        stdout_path = log_dir / f"{name}.stdout"
        stderr_path = log_dir / f"{name}.stderr"
        out = stdout_path.open("w")
        err = stderr_path.open("w")
        popen = subprocess.Popen(self.command(binary, args), stdout=out, stderr=err)
        return ManagedProcess(name, popen, stdout_path, stderr_path,
                              time.monotonic(), out, err)

    def setup(self, run_dir: Path) -> None:
        pass

    def teardown(self) -> None:
        pass

    def logs(self) -> str:
        return ""


ENGINE_PORT = 50051


class DockerLauncher(Launcher):
    """Runs the engine in a container and execs the other processes into it.

    All processes share the container's network namespace, so participants reach the
    engine on localhost with no port juggling, and the run directory is bind-mounted
    so artifacts land on the host as they are written.

    Signals need care. `docker exec` does NOT forward SIGTERM into the container, so
    terminating the client leaves the real process running and it is then killed
    outright at teardown -- meaning participants never cancel their resting orders and
    never write their metrics. Each process therefore records its in-container PID to
    a file on the shared mount, and we signal it with `docker exec ... kill`.
    """

    def __init__(self, image: str, run_dir_mount: Path, container_name: str,
                 engine_args: list[str] | None = None,
                 publish_port: int | None = None):
        super().__init__(Path("/app"))
        self.image = image
        self.run_dir_mount = Path(run_dir_mount).resolve()
        self.container_name = container_name
        # Passed to the engine when the container starts, overriding its CMD. This is
        # how a scenario controls observation resolution and registered symbols.
        self.engine_args = list(engine_args or [])
        # Host port published for recording runs, where a host-side observer (the
        # finchart adapter) has to reach the engine. The container always listens on
        # ENGINE_PORT; only the host side varies, so a recording run does not collide
        # with a docker-compose stack already holding 50051. Normal runs publish
        # nothing at all.
        self.publish_port = publish_port
        self._started = False

    def docker_run_argv(self) -> list[str]:
        """The `docker run` command line. Split out so ordering is testable.

        Every option must precede the image name; anything after it is passed to the
        container instead.
        """
        argv = ["docker", "run", "-d", "--name", self.container_name,
                "-v", f"{self.run_dir_mount}:/run"]
        if self.publish_port is not None:
            argv += ["-p", f"{self.publish_port}:{ENGINE_PORT}"]
        argv.append(self.image)
        if self.engine_args:
            argv += ["/app/matching_engine_server", *self.engine_args]
        return argv

    def command(self, name: str, args: list[str]) -> list[str]:
        return ["docker", "exec", self.container_name, f"/app/{name}", *args]

    def container_path(self, host_path: Path) -> str:
        rel = Path(host_path).resolve().relative_to(self.run_dir_mount)
        return str(Path("/run") / rel) if str(rel) != "." else "/run"

    def setup(self, run_dir: Path) -> None:
        if shutil.which("docker") is None:
            raise LaunchError("docker is not on PATH; use --backend local instead")
        subprocess.run(["docker", "rm", "-f", self.container_name],
                       capture_output=True, check=False)
        proc = subprocess.run(self.docker_run_argv(), capture_output=True, text=True)
        if proc.returncode != 0:
            raise LaunchError(f"could not start engine container: {proc.stderr.strip()}")
        self._started = True
        (self.run_dir_mount / "pids").mkdir(exist_ok=True)

    def start(self, name: str, binary: str, args: list[str], log_dir: Path) -> ManagedProcess:
        log_dir.mkdir(parents=True, exist_ok=True)
        stdout_path = log_dir / f"{name}.stdout"
        stderr_path = log_dir / f"{name}.stderr"
        out = stdout_path.open("w")
        err = stderr_path.open("w")

        pid_file = f"/run/pids/{name}.pid"
        inner = " ".join(shlex.quote(a) for a in [f"/app/{binary}", *args])
        # `exec` replaces the shell, so the recorded PID is the binary's own.
        wrapper = f"echo $$ > {shlex.quote(pid_file)}; exec {inner}"
        argv = ["docker", "exec", self.container_name, "sh", "-c", wrapper]

        popen = subprocess.Popen(argv, stdout=out, stderr=err)

        host_pid_file = self.run_dir_mount / "pids" / f"{name}.pid"

        def signaller(sig: int, _f=host_pid_file) -> None:
            for _ in range(50):  # the wrapper may not have written it yet
                if _f.exists() and _f.read_text().strip():
                    break
                time.sleep(0.1)
            if not _f.exists():
                return
            pid = _f.read_text().strip()
            if not pid:
                return
            signame = {signal.SIGTERM: "TERM", signal.SIGKILL: "KILL",
                       signal.SIGINT: "INT"}.get(sig, str(int(sig)))
            # Must go through `sh -c`: the slim runtime image has no /bin/kill, and
            # `docker exec` needs a real executable -- it cannot run a shell builtin.
            subprocess.run(
                ["docker", "exec", self.container_name, "sh", "-c",
                 f"kill -{signame} {shlex.quote(pid)}"],
                capture_output=True, check=False,
            )

        return ManagedProcess(name, popen, stdout_path, stderr_path,
                              time.monotonic(), out, err, signaller=signaller)

    def teardown(self) -> None:
        if self._started:
            subprocess.run(["docker", "rm", "-f", self.container_name],
                           capture_output=True, check=False)
            self._started = False

    def logs(self) -> str:
        proc = subprocess.run(["docker", "logs", self.container_name],
                              capture_output=True, text=True, check=False)
        return proc.stdout + proc.stderr


@dataclass
class Supervisor:
    """Tracks running processes and guarantees they are all stopped."""

    processes: list[ManagedProcess] = field(default_factory=list)

    def add(self, proc: ManagedProcess) -> ManagedProcess:
        self.processes.append(proc)
        return proc

    def crashed(self) -> list[ManagedProcess]:
        out = []
        for proc in self.processes:
            code = proc.poll()
            if code is not None and code != 0 and proc.stop_requested_at is None:
                out.append(proc)
        return out

    def stop(self, proc: ManagedProcess, grace_seconds: float = 10.0) -> int | None:
        """SIGTERM, then SIGKILL after the grace period.

        The grace period matters: a participant uses it to cancel its resting orders
        and write its metrics. Killing it early leaves orders in the book and
        invalidates the run.
        """
        if proc.poll() is not None:
            proc.close_files()
            return proc.popen.returncode

        proc.stop_requested_at = time.monotonic()
        proc.send(signal.SIGTERM)

        deadline = time.monotonic() + grace_seconds
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                proc.close_files()
                return proc.popen.returncode
            time.sleep(0.1)

        proc.killed = True
        proc.send(signal.SIGKILL)
        try:
            proc.popen.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                proc.popen.kill()
            except ProcessLookupError:
                pass
        proc.close_files()
        return proc.popen.returncode

    def stop_all(self, grace_seconds: float = 10.0) -> None:
        for proc in reversed(self.processes):
            self.stop(proc, grace_seconds)
