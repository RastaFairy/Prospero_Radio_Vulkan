#!/usr/bin/env bash
set -eu
python3 - <<'PYEOF'
import ast
src = open('/mnt/d/prospero_modern/overlay/apply-vulkan.py', encoding='utf-8').read()
tree = ast.parse(src)
for node in ast.walk(tree):
    if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', '') == 'CPP_RUNTIME_SOURCE':
        open('/tmp/app_cpp_runtime.cpp', 'w').write(node.value.value)
        print('extracted')
PYEOF
g++ -std=c++17 -fsyntax-only -I/usr/include/c++/v1 /tmp/app_cpp_runtime.cpp 2>&1 | head -20 || true
g++ -std=c++17 -fsyntax-only /tmp/app_cpp_runtime.cpp && echo "SYNTAX OK"
