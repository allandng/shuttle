# Two-platform harness: every leg is one command (Phase 0 debugging strategy).
#   make test-mac    — native macOS build + tests under ASan+UBSan
#   make test-linux  — same inside the glibc arm64 container
#   make tsan-mac / tsan-linux — TSan legs (separate build trees; ASan and
#                      TSan cannot be linked together)
#
# --shm-size=512m: Docker's default 64 MB /dev/shm cannot hold the >100 MB
# segments implied by 50 MB payloads + the SRS 2x capacity rule; ftruncate/
# mmap would fail with a confusing ENOSPC/EINVAL.
#
# setarch -R on linux TSan runs: gcc-13 TSan vs Ubuntu 24.04 ASLR entropy
# workaround (documented in PROGRESS.md; pre-approved, not a suppression).
# setarch needs personality(2), which Docker's default seccomp profile blocks
# (ADDR_NO_RANDOMIZE arg is not on the allowlist), so the TSan leg runs with
# seccomp=unconfined. Dev harness running our own code only.

IMAGE := shuttle-linux-dev
DOCKER_RUN := docker run --rm --shm-size=512m -v "$(CURDIR)":/work -w /work $(IMAGE)
DOCKER_RUN_TSAN := docker run --rm --shm-size=512m --security-opt seccomp=unconfined -v "$(CURDIR)":/work -w /work $(IMAGE)

.PHONY: test-mac test-linux tsan-mac tsan-linux docker-image clean

test-mac:
	cmake -B build/mac-asan -DSHUTTLE_SAN=asan
	cmake --build build/mac-asan -j
	ctest --test-dir build/mac-asan --output-on-failure

tsan-mac:
	cmake -B build/mac-tsan -DSHUTTLE_SAN=tsan
	cmake --build build/mac-tsan -j
	ctest --test-dir build/mac-tsan --output-on-failure

docker-image:
	docker build -t $(IMAGE) docker

test-linux: docker-image
	$(DOCKER_RUN) bash -c 'cmake -B build/linux-asan -DSHUTTLE_SAN=asan \
	  && cmake --build build/linux-asan -j \
	  && ctest --test-dir build/linux-asan --output-on-failure'

tsan-linux: docker-image
	$(DOCKER_RUN_TSAN) bash -c 'cmake -B build/linux-tsan -DSHUTTLE_SAN=tsan \
	  && cmake --build build/linux-tsan -j \
	  && setarch $$(uname -m) -R ctest --test-dir build/linux-tsan --output-on-failure'

clean:
	rm -rf build
