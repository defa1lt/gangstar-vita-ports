#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
test_dir=$(mktemp -d "$project_dir/build/host-tests.XXXXXX")
python3 tools/prepare_compat.py --out "$test_dir/compat"
python3 tools/game_profile.py --header "$test_dir/game_profile.h"
python3 tools/prepare_vitagl.py --out "$test_dir/vitaGL"
clang -std=c11 -Wall -Wextra -Werror -I"$test_dir/vitaGL/source" \
  source/tests/pixel_layout.c -o "$test_dir/pixel-layout"
"$test_dir/pixel-layout"
clang -std=c11 -Wall -Wextra -Werror -I"$test_dir/vitaGL/source/utils" \
  source/tests/vertex_layout.c -o "$test_dir/vertex-layout"
"$test_dir/vertex-layout"
clang -std=gnu11 -Wall -Wextra -Werror -Iinclude -Ithird_party/mc2-loader/source \
  source/tests/runtime_io.c source/md5.c source/memory.c source/epoll.c \
  -pthread -o "$test_dir/runtime-io"
"$test_dir/runtime-io"
clang -std=gnu11 -Wall -Wextra -Werror -Iinclude source/tests/offline.c source/offline.c -o "$test_dir/offline"
python3 - "$test_dir" <<'PY_EXTRACT'
from pathlib import Path
import sys,re
out=Path(sys.argv[1])
source=(out/'compat/reimpl/io.c').read_text()
start=source.index('static const char *translate_bundle_path(')
end=source.index('\n}\n',start)+3
snippet=source[start:end]
start=source.index('int fcntl_soloader(')
end=source.index('\n}\n',start)+3
converter=Path('third_party/mc2-loader/source/reimpl/bits/_struct_converters.c').read_text()
flag_start=converter.index('#define SC_INLINE')
flag_end=converter.index('\n}\n',flag_start)+3
snippet+=converter[flag_start:flag_end]+source[start:end]
java=Path('source/java.c').read_text()
identity_start=java.index('static const char device_identity[]')
identity=java[identity_start:java.index('\n',identity_start)+1]
start=java.index('static char *expand_template(')
end=java.index('\n}\n',start)+3
snippet+=java[start:end]
integer_start=java.index('static jobject integers(')
integer_end=java.index('\n}\n',integer_start)+3
(out/'java-integers-under-test.c').write_text('#include "game_profile.h"\nenum {GLUID=100,GLUID_DEFAULT=101};\n'+identity+java[integer_start:integer_end])
string_start=java.index('static jobject string_value(')
string_end=java.index('\n}\n',string_start)+3
string_body=java[string_start:string_end]
names=re.findall(r'case (STR_\w+):',string_body)
string_enum='enum {'+','.join(f'{name}={110+i}' for i,name in enumerate(names))+'};\n'
(out/'java-strings-under-test.c').write_text('#define ROOT "ux0:data/gangstarvegas/"\n#define CONTEXT ((jobject)0x42424242)\n'+string_enum+string_body)
bindings=[(kind,name) for kind,name in re.findall(r'OBJECT\((STR_\w+),\s*("[^"]+")\)',java)
          if kind in ('STR_HDIDFV','STR_HDIDFV_VERSION','STR_DEVICE_GLDID') or
          (kind=='STR_ID' and '/SendInfo/getGLDID(' in name)]
assert len(bindings)==4
(out/'java-string-bindings-under-test.h').write_text(''.join('{'+kind+','+name+',METHOD_TYPE_OBJECT},\n' for kind,name in bindings))
(out/'paths-under-test.c').write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <limits.h>\n#include <fcntl.h>\n#include <errno.h>\n#include <stdarg.h>\n#include "md5.h"\n#define DATA_PATH "ux0:data/gangstarvegas/"\n'+identity+snippet)
PY_EXTRACT
clang -std=gnu11 -Wall -Wextra -Werror -Iinclude -I"$test_dir" source/tests/paths.c source/md5.c -o "$test_dir/paths"
"$test_dir/paths"
"$test_dir/offline"
clang -std=gnu11 -Wall -Wextra -Werror -Iinclude -Ithird_party/mc2-loader/lib \
  -c source/bundle.c -o "$test_dir/bundle.o"
clang -std=gnu11 -w -Iinclude -Ithird_party/mc2-loader/lib \
  -Ithird_party/mc2-loader/lib/falso_jni -I"$test_dir" source/tests/bundle.c source/md5.c "$test_dir/bundle.o" \
  "$test_dir/compat/falso_jni/FalsoJNI.c" "$test_dir/compat/falso_jni/FalsoJNI_ImplBridge.c" \
  third_party/mc2-loader/lib/falso_jni/converter.c -pthread -o "$test_dir/bundle-test"
cd "$test_dir"
./bundle-test
./bundle-test reload
./bundle-test interrupt
./bundle-test reload
