#!/usr/bin/env bash
# Runs Ghidra's headless analyzer directly through java.
# (analyzeHeadless.bat cannot handle the spaces in this project's path.)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -W)"
GHIDRA="$ROOT/tools/bin/ghidra_12.1.4_PUBLIC"
JAVA="$ROOT/tools/bin/jdk-21.0.12.1+1/bin/java.exe"
exec "$JAVA" \
  -Djava.system.class.loader=ghidra.GhidraClassLoader -Dfile.encoding=UTF8 \
  -Duser.country=US -Duser.language=en -Dsun.java2d.d3d=false \
  -Xmx12G -XX:+UseG1GC \
  -cp "$GHIDRA/Ghidra/Framework/Utility/lib/Utility.jar" \
  ghidra.Ghidra ghidra.app.util.headless.AnalyzeHeadless "$@"
