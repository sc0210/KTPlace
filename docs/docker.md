# Using KTPlace with Docker

A task-oriented guide to the Docker environment: start it, build the engine,
and -- most of all -- run commands in it. The [README](../README.md#any-os-the-docker-environment)
explains *why* it is laid out this way; this page is the *how*.

All you need is Docker (Docker Desktop on macOS and Windows). Every command
below runs from the repository root on the host unless it says "inside".

## The pieces

| Container     | Image target | What it is for                                     | Reach it at                     |
|---------------|--------------|----------------------------------------------------|---------------------------------|
| `ktplace-dev` | `dev`        | Build, test and run; the repository is live at `/workspace` | `ssh -p 2222 dev@127.0.0.1` |
| `ktplace-web` | `web`        | Browser console: start runs, watch them, download results   | http://127.0.0.1:8080        |

Both share one build volume, so the `build/bin/ktplace` the dev container
compiles is the binary the console runs. Both ports are bound to `127.0.0.1`
only -- nothing is exposed to the network.

## Start, stop, rebuild

With a bash shell (macOS, Linux, WSL, Git Bash), use `scripts/devenv.sh`:

```sh
scripts/devenv.sh up              # start both containers, wait for SSH, compile ktplace
scripts/devenv.sh up --no-build   # start without compiling
scripts/devenv.sh build           # recompile later (args go to make: build -j8 test)
scripts/devenv.sh status          # what is running, on which ports
scripts/devenv.sh stop            # stop; containers and volumes kept
scripts/devenv.sh down            # remove containers; volumes (builds, runs) kept
```

Without bash -- PowerShell included -- use compose directly, then compile once
inside:

```sh
docker compose up -d --build                           # both containers
docker exec -u dev ktplace-dev bash -lc 'make -j"$(nproc)"'   # build the engine
docker compose stop                                    # or: docker compose down
```

A rebuild is picked up by the console on its next run; it needs no restart.

## Running commands

There are four ways in, from most to least interactive.

### 1. A shell in the dev container

```sh
scripts/devenv.sh ssh                          # or: ssh -p 2222 dev@127.0.0.1
scripts/devenv.sh exec                         # same, through docker exec (no SSH key needed)
docker exec -it -u dev ktplace-dev bash -l     # what `exec` runs, for PowerShell
```

You land in `/workspace` (the repository) with `ktplace` on `PATH`:

```sh
make -j"$(nproc)" && make test
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1-ntu -a ntuplace1 -v
```

Anything written under `output/` appears on the host straight away.

### 2. One command, without a session

Pass the command as a string; it runs in `/workspace`, the same as a login:

```sh
scripts/devenv.sh ssh 'make test'
scripts/devenv.sh exec 'ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1'
docker exec -u dev ktplace-dev bash -lc 'make -j"$(nproc)" && make test'
```

With the `~/.ssh/config` entry from `scripts/devenv.sh ssh-config`, plain
`ssh ktplace-dev 'make test'`, `scp`, `rsync` and VS Code *Remote-SSH* all work
too.

### 3. The engine's options

```
ktplace <input_dir> [options]
  -a, --algorithm <name>   simpl (default) or ntuplace1
  -w, --work-dir <dir>     where placed.pl, plots/ and ktplace.log go
  -v, --verbose            also print trace diagnostics (ktplace_trace.log is always written)
  -h, --help / -V, --version
```

Tuning is through `KTPLACE_*` environment variables, not flags. Set them on
the command:

```sh
KTPLACE_SIMPL_ITERS=25 KTPLACE_SIMPL_SEED=1 \
    ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1-25it
```

Common ones: `KTPLACE_SIMPL_ITERS`, `KTPLACE_SIMPL_SEED`, `KTPLACE_SIMPL_GRID`,
`KTPLACE_SIMPL_DENSITY`, `KTPLACE_DP_WINDOW`, `KTPLACE_ANIM` (animated GIF),
`KTPLACE_FINAL_ZOOM`. `grep -rhoE 'KTPLACE_[A-Z0-9_]+' src | sort -u` lists
them all.

### 4. From the browser: the web console

Open http://127.0.0.1:8080.

- **Start a run**: pick a design (every suite under `benchmark/` is mounted),
  an algorithm, optionally *verbose*, and `KEY=VALUE` lines in
  *Environment overrides* (only `KTPLACE_*` keys with plain values are kept).
  The transcript streams live with a phase bar and a heartbeat (frames, CPU,
  memory), so a long solve is visibly alive.
- **Review**: open the SVG gallery, the final image and the logs; download the
  transcript, trace, `placed.pl`, `request.json`, or *everything (.zip)*.
- **History**: every run with status, time, HPWL and verdict; label or delete
  runs there. Runs live in the `web-runs` volume and survive `down`/`up`.

**The Terminal panel** runs a one-shot command in the *console* container and
streams its output. Use it for quick checks next to your runs:

```sh
ls runs/                                            # every run directory
tail -n 30 runs/<id>/ktplace.log
build/bin/ktplace benchmark/ISPD_2005/adaptec1 -w runs/manual-1 -a simpl
KTPLACE_SIMPL_SEED=7 build/bin/ktplace benchmark/ISPD_2005/adaptec1 -w runs/seed7
```

Know its limits:

- It starts in `/ktplace`. The engine is `build/bin/ktplace` (the shared build
  volume), not on `PATH` as it is in the dev container.
- No terminal: interactive programs (`vim`, `top`, `less`) will not work; pipe
  through `head` or `tail` instead.
- No compiler: build in the dev container (way 1 or 2).
- At most three commands at once. Output is capped at 64 MB and a command at
  one hour; past either it is stopped with a note at the end of its output.
  *Stop* sends SIGTERM, then SIGKILL five seconds later.
- Runs started here are not in the console's history -- only runs started from
  the form are. Their files are still under `runs/`.

The panel is enabled by `KTPLACE_WEB_ALLOW_EXEC=1`, which `compose.yaml` sets.
Remove that line to switch it off.

## Calling the console's API

The page is a thin client over a JSON API, so scripts can drive it too:

```sh
H='Content-Type: application/json'
curl -s http://127.0.0.1:8080/api/benchmarks
curl -s -X POST -H "$H" http://127.0.0.1:8080/api/runs \
     -d '{"benchmark":"ISPD_2005/adaptec1","algorithm":"simpl","env":{"KTPLACE_SIMPL_SEED":"1"}}'
curl -s "http://127.0.0.1:8080/api/runs/<id>/log?offset=0"
curl -s -o run.zip "http://127.0.0.1:8080/api/runs/<id>/download?what=all"
curl -s -X POST -H "$H" http://127.0.0.1:8080/api/exec -d '{"cmd":"ls runs"}'
curl -s "http://127.0.0.1:8080/api/exec/<id>?offset=0"
```

The full endpoint list is at the top of [`webui/server.py`](../webui/server.py).
Log and command output come back at most 1 MB per call: keep requesting with
the returned `offset` until `done` is true.

Because the console can run commands, it only accepts requests that look like
they came from this machine:

- The `Host` header must be `localhost`, `127.0.0.1`, `[::1]` or `ktplace-web`.
  To reach it under another name (a LAN IP, a tunnel), list the names in
  `KTPLACE_WEB_ALLOWED_HOSTS=name1,name2`.
- POST and DELETE requests from a page on another origin are refused, and
  every POST must be `Content-Type: application/json` (otherwise HTTP 415).

## Without compose

```sh
# CI-style image: builds and tests a copy of the tree
docker build -t ktplace .
docker run -v "$PWD/output:/ktplace/output" ktplace \
    ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1

# Small run-only image (~112 MB); ktplace is the entrypoint
docker build --target run -t ktplace-run .
docker run --rm -v "$PWD/output:/ktplace/output" ktplace-run \
    benchmark/ISPD_2005/adaptec1 -w output/adaptec1

# Console by hand, against the compose build volume
docker build --target web -t ktplace-web .
docker run --rm -p 127.0.0.1:8080:8080 -e KTPLACE_WEB_ALLOW_EXEC=1 \
    -v ktplace_build:/ktplace/build -v ktplace_web-runs:/ktplace/runs ktplace-web
```

## Settings

| Variable                     | Default   | Effect                                         |
|------------------------------|-----------|------------------------------------------------|
| `KTPLACE_SSH_PORT`           | `2222`    | Host port for the dev container's SSH          |
| `KTPLACE_SSH_PUBKEY`         | `~/.ssh/id_ed25519.pub` | Key installed for `dev`          |
| `KTPLACE_WEB_PORT`           | `8080`    | Host port for the console                      |
| `KTPLACE_WEB_ALLOW_EXEC`     | off (`1` in compose) | Enables the Terminal panel          |
| `KTPLACE_WEB_ALLOWED_HOSTS`  | --        | Extra host names the console answers to        |
| `KTPLACE_WEB_MAX_RUNS`       | `1`       | Placements allowed at once                     |
| `KTPLACE_WEB_EXEC_MAX_BYTES` | 64 MB     | Output cap per Terminal command                |
| `KTPLACE_WEB_EXEC_TIMEOUT`   | `3600`    | Seconds before a Terminal command is stopped   |
| `KTPLACE_BIN`                | --        | Force a specific engine binary for the console |

Set the ports on the host when starting (`KTPLACE_WEB_PORT=9090 scripts/devenv.sh up`);
the console settings go under `environment:` for `web` in `compose.yaml`.

## Troubleshooting

- **"ktplace is not built yet"** in the console: run `scripts/devenv.sh build`
  (or `make` inside the dev container), then start the run again.
- **SSH: "REMOTE HOST IDENTIFICATION HAS CHANGED"**: the host-key volume was
  recreated. `ssh-keygen -R ktplace-dev`, then retry.
- **SSH refuses your key**: after changing `KTPLACE_SSH_PUBKEY` or your key,
  run `scripts/devenv.sh down` and `up` -- the key is copied in at start.
- **Console returns 403 "unexpected Host header"**: you opened it under a name
  not in the allow-list; use `127.0.0.1`/`localhost` or set
  `KTPLACE_WEB_ALLOWED_HOSTS`.
- **Logs**: `docker logs ktplace-dev`, `docker logs ktplace-web`.
- **Windows**: keep the clone inside WSL 2 rather than on `C:`; bind mounts from
  the Windows filesystem are slow.
