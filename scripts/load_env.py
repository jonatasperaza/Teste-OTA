from pathlib import Path
import os
import re

Import("env")

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
ENV_FILE = PROJECT_DIR / ".env"
OUT_FILE = PROJECT_DIR / "include" / "generated_secrets.h"

REQUIRED_KEYS = [
    "WIFI_SSID",
    "WIFI_PASS",
    "TB_HOST",
    "TB_PORT",
    "TB_TOKEN",
    "FW_TITLE",
    "FW_VERSION",
]

DEFAULTS = {
    "TB_PORT": "1883",
    "TB_HOST": "mytb.fabricadesoftware.ifc.edu.br",
    "FW_TITLE": "esp32-tb-ota-test",
    "FW_VERSION": "1.0.0",
}


def parse_env_file(path: Path) -> dict:
    values = {}

    if not path.exists():
        return values

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()

        if not line or line.startswith("#"):
            continue

        if "=" not in line:
            continue

        key, value = line.split("=", 1)

        key = key.strip()
        value = value.strip()

        # Remove aspas simples ou duplas, se existirem
        if len(value) >= 2:
            if (value[0] == value[-1]) and value[0] in ("'", '"'):
                value = value[1:-1]

        values[key] = value

    return values


def cpp_string(value: str) -> str:
    value = value.replace("\\", "\\\\")
    value = value.replace('"', '\\"')
    return f'"{value}"'


def is_int(value: str) -> bool:
    return re.fullmatch(r"\d+", value or "") is not None


values = {}
values.update(DEFAULTS)
values.update(parse_env_file(ENV_FILE))

# Variáveis de ambiente do sistema/GitHub Actions têm prioridade.
for key in REQUIRED_KEYS:
    if os.getenv(key):
        values[key] = os.getenv(key)

missing = [key for key in REQUIRED_KEYS if not values.get(key)]

if missing:
    raise RuntimeError(
        "Variáveis ausentes: "
        + ", ".join(missing)
        + ". Crie um arquivo .env na raiz do projeto ou defina essas variáveis no ambiente."
    )

OUT_FILE.parent.mkdir(parents=True, exist_ok=True)

lines = [
    "#pragma once",
    "",
    "// Arquivo gerado automaticamente por scripts/load_env.py",
    "// Não edite manualmente e não suba para o Git.",
    "",
]

for key in REQUIRED_KEYS:
    value = str(values[key])

    if key == "TB_PORT":
        if not is_int(value):
            raise RuntimeError(f"TB_PORT precisa ser número. Valor recebido: {value}")

        lines.append(f"#define {key} {value}")
    else:
        lines.append(f"#define {key} {cpp_string(value)}")

lines.append("")

OUT_FILE.write_text("\n".join(lines), encoding="utf-8")

print(f"[ENV] Gerado: {OUT_FILE}")
print("[ENV] Variáveis carregadas: " + ", ".join(REQUIRED_KEYS))
