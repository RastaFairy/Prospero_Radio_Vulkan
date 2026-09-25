# FIX4 — regression gate

Expected bootstrap order:
`make deps` → `fetch-sources.py` → `build-psbc-ps5.sh` → `adapt-opengl-sdk.sh` → `fetch-mesa.sh` → `build-vulkan-runtime.sh` → `build-driver.sh`.

The key regression gate is that `third_party/opengnm-psbc` must exist before `build-psbc-ps5.sh` invokes `fetch-sources.py --verify-psbc`.
