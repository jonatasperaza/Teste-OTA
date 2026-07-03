# ESP32 OTA via ThingsBoard + PlatformIO + GitHub Actions

Este projeto demonstra como fazer atualização OTA em um ESP32 usando o ThingsBoard como servidor de firmware. O fluxo completo permite:

```txt
ESP32 conecta no ThingsBoard via MQTT
ThingsBoard envia atributos OTA
ESP32 baixa o firmware em chunks
ESP32 grava o firmware usando Update.h
ESP32 reinicia atualizado
GitHub Actions compila e envia o firmware automaticamente para o ThingsBoard
```

## 1. Visão geral

O fluxo final fica assim:

```txt
git push na branch main
        ↓
semantic-release calcula a próxima versão
        ↓
GitHub Actions compila o firmware com PlatformIO
        ↓
Workflow gera firmware.bin
        ↓
Script calcula SHA-256
        ↓
Script cria um pacote OTA no ThingsBoard
        ↓
Script faz upload do firmware.bin
        ↓
Opcionalmente vincula o pacote ao device
        ↓
ESP32 recebe atualização e instala
```

O ThingsBoard não “empurra” diretamente o firmware para o ESP32. Ele informa ao dispositivo que existe uma nova versão por meio de atributos compartilhados, e o ESP32 baixa os pedaços do firmware via MQTT.

---

# 2. Requisitos

## Hardware

* ESP32 DOIT DevKit V1 ou placa compatível.
* Cabo USB para primeira gravação.
* Wi-Fi disponível.
* Servidor ThingsBoard acessível pelo ESP32.

## Software

* PlatformIO.
* Conta no ThingsBoard.
* GitHub Actions, caso queira automatizar o build.
* Python 3.
* Node.js/npm para usar semantic-release.

---

# 3. Criando o device no ThingsBoard

No ThingsBoard:

```txt
Entities → Devices → Add device
```

Crie um device, por exemplo:

```txt
esp32-ota-test
```

Depois vá em:

```txt
Device → Manage credentials
```

Copie o Access Token do device.

Esse token será usado pelo ESP32 como usuário MQTT.

Exemplo:

```txt
TB_TOKEN=seu_token_do_device
```

---

# 4. Testando MQTT manualmente

Antes de testar OTA, confirme se o ThingsBoard está recebendo telemetria:

```bash
mosquitto_pub -d -q 1 \
  -h mytb.seudominio.com \
  -p 1883 \
  -t v1/devices/me/telemetry \
  -u "SEU_TOKEN_DO_DEVICE" \
  -m '{"temperature":25}'
```

No ThingsBoard, abra o device e vá em:

```txt
Latest telemetry
```

Deve aparecer:

```txt
temperature = 25
```

Caso o device apareça ativo, mas a telemetria não apareça, verifique a Rule Chain. A Root Rule Chain precisa ter um node que salva telemetria, normalmente chamado:

```txt
Save Timeseries
```

Sem esse node, o ThingsBoard recebe a mensagem, mas não salva em `Latest telemetry`.

---

# 5. Estrutura do projeto

A estrutura recomendada é:

```txt
.
├── .env
├── .env.example
├── .gitignore
├── package.json
├── platformio.ini
├── partitions_4mb_ota.csv
├── .releaserc.json
├── include/
│   └── generated_secrets.h
├── scripts/
│   ├── load_env.py
│   └── build_and_upload_ota.py
├── src/
│   └── main.cpp
└── .github/
    └── workflows/
        └── esp32-release.yml
```

O arquivo `.env` não deve ir para o GitHub.

---

# 6. Arquivo `.env`

Crie um arquivo `.env` na raiz do projeto:

```env
WIFI_SSID=NomeDoSeuWifi
WIFI_PASS=SenhaDoSeuWifi

TB_HOST=mytb.seudominio.com
TB_PORT=1883
TB_TOKEN=token_do_device

FW_TITLE=esp32-tb-ota-test
FW_VERSION=1.0.0
```

`FW_TITLE` precisa ser exatamente igual ao título do firmware que será cadastrado no ThingsBoard.

---

# 7. Arquivo `.env.example`

Este arquivo pode ir para o GitHub:

```env
WIFI_SSID=
WIFI_PASS=

TB_HOST=mytb.seudominio.com
TB_PORT=1883
TB_TOKEN=

FW_TITLE=esp32-tb-ota-test
FW_VERSION=1.0.0
```

---

# 8. `.gitignore`

Use:

```gitignore
.pio
.vscode/.browse.c_cpp.db*
.vscode/c_cpp_properties.json
.vscode/launch.json
.vscode/ipch
.env
/node_modules
/include/generated_secrets.h

```

---

# 9. `platformio.ini`

Exemplo para ESP32 DOIT DevKit V1:

```ini
[env:esp32doit-devkit-v1]
platform = espressif32
board = esp32doit-devkit-v1
framework = arduino

monitor_speed = 115200
upload_speed = 921600

board_build.partitions = partitions_4mb_ota.csv

extra_scripts =
  pre:scripts/load_env.py

lib_deps =
  knolleary/PubSubClient@^2.8
  bblanchon/ArduinoJson@^7

build_flags =
  -D MQTT_MAX_PACKET_SIZE=8192
```

As senhas e tokens não ficam no `platformio.ini`. Elas são carregadas do `.env`.

---

# 10. Script para carregar `.env`

Crie `scripts/load_env.py`:

```python
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
    "TB_HOST": "mytb.seudominio.com",
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

for key in REQUIRED_KEYS:
    if os.getenv(key):
        values[key] = os.getenv(key)

missing = [key for key in REQUIRED_KEYS if not values.get(key)]

if missing:
    raise RuntimeError(
        "Variáveis ausentes: "
        + ", ".join(missing)
        + ". Crie um arquivo .env ou defina essas variáveis no ambiente."
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
```

Esse script gera automaticamente:

```txt
include/generated_secrets.h
```

---

# 11. Firmware ESP32

O firmware deve incluir:

```cpp
#include "generated_secrets.h"
```

Assim o código pode usar:

```cpp
WIFI_SSID
WIFI_PASS
TB_HOST
TB_PORT
TB_TOKEN
FW_TITLE
FW_VERSION
```

O ESP32 deve:

1. Conectar no Wi-Fi.
2. Conectar no MQTT usando o token do ThingsBoard.
3. Publicar telemetria.
4. Solicitar atributos compartilhados.
5. Escutar atributos OTA.
6. Baixar os chunks do firmware.
7. Gravar com `Update.write()`.
8. Validar checksum SHA-256.
9. Reiniciar.

Tópicos usados:

```txt
v1/devices/me/telemetry
v1/devices/me/attributes
v1/devices/me/attributes/request/{requestId}
v1/devices/me/attributes/response/+
v2/fw/request/{requestId}/chunk/{chunkIndex}
v2/fw/response/+/chunk/+
```

---

# 12. Primeiro upload via USB

Antes de OTA funcionar, suba o firmware base por USB:

```bash
pio run -t upload
pio device monitor
```

No monitor serial, o ESP deve mostrar algo como:

```txt
[FW] Title: esp32-tb-ota-test
[FW] Version: 1.0.0
[WiFi] OK
[MQTT] Conectado
[TB] Telemetria enviada
```

No ThingsBoard, confira em:

```txt
Device → Latest telemetry
```

---

# 13. Criando pacote OTA manualmente

Para testar manualmente:

1. Mude no `.env`:

```env
FW_VERSION=1.0.1
```

2. Compile sem upload:

```bash
pio run
```

3. Pegue o arquivo:

```txt
.pio/build/esp32doit-devkit-v1/firmware.bin
```

4. No ThingsBoard, vá em:

```txt
Advanced features → OTA updates → Add package
```

Preencha:

```txt
Title: esp32-tb-ota-test
Version: 1.0.1
Device Profile: perfil do seu ESP32
Type: Firmware
File: firmware.bin
Checksum: SHA-256 ou automático
```

5. Depois vincule ao device:

```txt
Entities → Devices → seu device → Firmware
```

Selecione o firmware criado.

---

# 14. O que o ESP32 deve mostrar durante OTA

No serial:

```txt
[OTA] Atributos recebidos:
  fw_title: esp32-tb-ota-test
  fw_version: 1.0.1
  fw_size: ...

[OTA] Iniciando OTA via ThingsBoard
[OTA] Pedindo chunk 0
[OTA] Chunk 0 OK
[OTA] Pedindo chunk 1
...
[OTA] Download concluido
[OTA] Verificando SHA-256
[OTA] Firmware gravado com sucesso. Reiniciando...
```

Depois do reboot:

```txt
[FW] Version: 1.0.1
```

---

# 15. Integração com semantic-release

A ideia é fazer a versão do firmware ser gerada automaticamente pelos commits.

Exemplos:

```txt
fix: corrige timeout do OTA        → patch
feat: adiciona telemetria extra    → minor
BREAKING CHANGE: muda protocolo    → major
```

Instale dependências Node:

```bash
npm init -y
npm install -D semantic-release @semantic-release/commit-analyzer @semantic-release/release-notes-generator @semantic-release/github @semantic-release/exec
```

---

# 16. `package.json`

Exemplo:

```json
{
  "name": "esp32-tb-ota-test",
  "private": true,
  "version": "0.0.0-development",
  "scripts": {
    "release": "semantic-release"
  },
  "devDependencies": {
    "@semantic-release/commit-analyzer": "^13.0.1",
    "@semantic-release/exec": "^7.1.0",
    "@semantic-release/github": "^11.0.1",
    "@semantic-release/release-notes-generator": "^14.0.3",
    "semantic-release": "^25.0.5"
  }
}
```

---

# 17. `.releaserc.json`

```json
{
  "branches": ["main"],
  "tagFormat": "fw-${version}",
  "plugins": [
    "@semantic-release/commit-analyzer",
    "@semantic-release/release-notes-generator",
    [
      "@semantic-release/exec",
      {
        "publishCmd": "python scripts/build_and_upload_ota.py --version ${nextRelease.version}"
      }
    ],
    "@semantic-release/github"
  ]
}
```

Com isso, quando o semantic-release detectar uma nova versão, ele chama:

```bash
python scripts/build_and_upload_ota.py --version 1.1.3
```

---

# 18. Script para build e upload OTA

Crie `scripts/build_and_upload_ota.py`:

```python
import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

import requests


ENV_NAME = "esp32doit-devkit-v1"
FIRMWARE_PATH = Path(f".pio/build/{ENV_NAME}/firmware.bin")


def run(cmd, env=None):
    print(f"[CMD] {' '.join(cmd)}", flush=True)
    result = subprocess.run(cmd, env=env)

    if result.returncode != 0:
        sys.exit(result.returncode)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()

    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)

    return h.hexdigest()


def login(tb_url: str, username: str, password: str) -> str:
    response = requests.post(
        f"{tb_url.rstrip('/')}/api/auth/login",
        json={"username": username, "password": password},
        timeout=30,
    )

    if response.status_code >= 300:
        raise RuntimeError(
            f"Login ThingsBoard falhou: {response.status_code} {response.text}"
        )

    return response.json()["token"]


def create_ota_package(tb_url, jwt, title, version, device_profile_id):
    payload = {
        "title": title,
        "version": version,
        "tag": version,
        "type": "FIRMWARE",
        "deviceProfileId": {
            "entityType": "DEVICE_PROFILE",
            "id": device_profile_id,
        },
    }

    response = requests.post(
        f"{tb_url.rstrip('/')}/api/otaPackage",
        json=payload,
        headers={
            "Content-Type": "application/json",
            "X-Authorization": f"Bearer {jwt}",
        },
        timeout=30,
    )

    if response.status_code >= 300:
        raise RuntimeError(
            f"Erro criando OTA package: {response.status_code} {response.text}"
        )

    ota_id = response.json()["id"]["id"]

    print(f"[RELEASE] OTA package criado: {ota_id}", flush=True)

    return ota_id


def upload_ota_file(tb_url, jwt, ota_package_id, firmware_path, checksum):
    with firmware_path.open("rb") as f:
        response = requests.post(
            f"{tb_url.rstrip('/')}/api/otaPackage/{ota_package_id}",
            headers={
                "X-Authorization": f"Bearer {jwt}",
            },
            data={
                "checksum": checksum,
                "checksumAlgorithm": "SHA256",
            },
            files={
                "file": ("firmware.bin", f, "application/octet-stream"),
            },
            timeout=120,
        )

    if response.status_code >= 300:
        raise RuntimeError(
            f"Erro fazendo upload OTA: {response.status_code} {response.text}"
        )

    print(
        f"[RELEASE] Firmware .bin enviado ao ThingsBoard. OTA package id={ota_package_id}",
        flush=True,
    )


def assign_firmware_to_device(
    tb_url: str, jwt: str, device_id: str, ota_package_id: str
) -> None:
    headers = {
        "Content-Type": "application/json",
        "X-Authorization": f"Bearer {jwt}",
    }

    get_url = f"{tb_url.rstrip('/')}/api/device/{device_id}"

    response = requests.get(get_url, headers=headers, timeout=30)

    if response.status_code >= 300:
        raise RuntimeError(
            f"Erro buscando device {device_id}: {response.status_code} {response.text}"
        )

    device = response.json()

    device["firmwareId"] = {
        "entityType": "OTA_PACKAGE",
        "id": ota_package_id,
    }

    save_url = f"{tb_url.rstrip('/')}/api/device"

    response = requests.post(save_url, json=device, headers=headers, timeout=30)

    if response.status_code >= 300:
        raise RuntimeError(
            f"Erro vinculando firmware ao device: {response.status_code} {response.text}"
        )

    print(
        f"[RELEASE] Firmware OTA {ota_package_id} vinculado automaticamente ao device {device_id}",
        flush=True,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)

    args = parser.parse_args()

    version = args.version

    tb_url = os.getenv("TB_URL", "").strip()
    tb_username = os.getenv("TB_USERNAME", "").strip()
    tb_password = os.getenv("TB_PASSWORD", "").strip()
    tb_device_profile_id = os.getenv("TB_DEVICE_PROFILE_ID", "").strip()
    tb_device_id = os.getenv("TB_DEVICE_ID", "").strip()

    if not tb_url:
        raise RuntimeError("TB_URL não definido.")

    if not tb_url.startswith(("http://", "https://")):
        raise RuntimeError("TB_URL precisa começar com http:// ou https://.")

    if tb_url.rstrip("/").endswith("/api"):
        raise RuntimeError("TB_URL não deve terminar com /api.")

    if not tb_username:
        raise RuntimeError("TB_USERNAME não definido.")

    if not tb_password:
        raise RuntimeError("TB_PASSWORD não definido.")

    if not tb_device_profile_id:
        raise RuntimeError("TB_DEVICE_PROFILE_ID não definido.")

    if not tb_device_id:
        raise RuntimeError("TB_DEVICE_ID não definido.")

    fw_title = os.getenv("FW_TITLE", "esp32-tb-ota-test")

    build_env = os.environ.copy()
    build_env["FW_VERSION"] = version
    build_env["FW_TITLE"] = fw_title

    print(f"[RELEASE] Firmware title: {fw_title}", flush=True)
    print(f"[RELEASE] Firmware version: {version}", flush=True)

    run(["pio", "run", "-e", ENV_NAME], env=build_env)

    if not FIRMWARE_PATH.exists():
        raise RuntimeError(f"Firmware não encontrado: {FIRMWARE_PATH}")

    checksum = sha256_file(FIRMWARE_PATH)

    print(f"[RELEASE] Firmware: {FIRMWARE_PATH}", flush=True)
    print(f"[RELEASE] Tamanho: {FIRMWARE_PATH.stat().st_size} bytes", flush=True)
    print(f"[RELEASE] SHA256: {checksum}", flush=True)

    jwt = login(tb_url, tb_username, tb_password)

    ota_id = create_ota_package(
        tb_url=tb_url,
        jwt=jwt,
        title=fw_title,
        version=version,
        device_profile_id=tb_device_profile_id,
    )

    upload_ota_file(
        tb_url=tb_url,
        jwt=jwt,
        ota_package_id=ota_id,
        firmware_path=FIRMWARE_PATH,
        checksum=checksum,
    )

    assign_firmware_to_device(
        tb_url=tb_url,
        jwt=jwt,
        device_id=tb_device_id,
        ota_package_id=ota_id,
    )

    print("[RELEASE] OTA criado, enviado e atribuído ao device com sucesso", flush=True)


if __name__ == "__main__":
    main()
```

---

# 19. Secrets do GitHub Actions

No GitHub:

```txt
Settings → Secrets and variables → Actions → New repository secret
```

Crie:

```txt
WIFI_SSID
WIFI_PASS
TB_TOKEN
TB_URL
TB_USERNAME
TB_PASSWORD
TB_DEVICE_PROFILE_ID
TB_DEVICE_ID
```

Significado:

```txt
WIFI_SSID              Nome do Wi-Fi usado pelo ESP32
WIFI_PASS              Senha do Wi-Fi
TB_TOKEN               Token MQTT do device ThingsBoard
TB_URL                 URL base do ThingsBoard, com http:// ou https://
TB_USERNAME            Login do painel ThingsBoard
TB_PASSWORD            Senha do painel ThingsBoard
TB_DEVICE_PROFILE_ID   ID do Device Profile
TB_DEVICE_ID           ID do device que receberá o OTA automaticamente
```

Exemplo:

```txt
TB_URL=https://mytb.seudominio.com
```

Não use:

```txt
mytb.seudominio.com
```

Também não use:

```txt
https://mytb.seudominio.com/api
```

O script já adiciona `/api/...`.

---

# 20. GitHub Actions

Crie `.github/workflows/esp32-release.yml`:

```yaml
name: ESP32 Release OTA

on:
  push:
    branches:
      - main

permissions:
  contents: write
  issues: write
  pull-requests: write

jobs:
  release:
    runs-on: ubuntu-latest

    env:
      FW_TITLE: esp32-tb-ota-test

      WIFI_SSID: ${{ secrets.WIFI_SSID }}
      WIFI_PASS: ${{ secrets.WIFI_PASS }}

      TB_HOST: mytb.seudominio.com
      TB_PORT: 1883
      TB_TOKEN: ${{ secrets.TB_TOKEN }}

      TB_URL: ${{ secrets.TB_URL }}
      TB_USERNAME: ${{ secrets.TB_USERNAME }}
      TB_PASSWORD: ${{ secrets.TB_PASSWORD }}
      TB_DEVICE_PROFILE_ID: ${{ secrets.TB_DEVICE_PROFILE_ID }}
      TB_DEVICE_ID: ${{ secrets.TB_DEVICE_ID }}

      GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}

    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with:
          fetch-depth: 0

      - name: Setup Node.js
        uses: actions/setup-node@v4
        with:
          node-version: "22"

      - name: Setup Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.12"

      - name: Install Python deps
        run: |
          pip install platformio requests

      - name: Install Node deps
        run: |
          npm ci

      - name: Semantic release
        run: |
          npm run release
```

---

# 21. Como criar uma nova versão

Use Conventional Commits.

Patch:

```bash
git commit -m "fix: corrige timeout no OTA"
git push origin main
```

Minor:

```bash
git commit -m "feat: adiciona nova telemetria de status"
git push origin main
```

Major:

```bash
git commit -m "feat: muda protocolo OTA" -m "BREAKING CHANGE: altera os atributos usados pelo firmware"
git push origin main
```

O semantic-release vai:

```txt
1. Calcular a versão.
2. Criar uma tag tipo fw-1.1.3.
3. Compilar o firmware.
4. Subir o pacote OTA no ThingsBoard.
5. Vincular o firmware ao device.
```

---

# 22. Logs esperados no GitHub Actions

Quando tudo estiver correto, o log deve mostrar:

```txt
The next release version is 1.1.3
Created tag fw-1.1.3
Call script python scripts/build_and_upload_ota.py --version 1.1.3
[RELEASE] Firmware title: esp32-tb-ota-test
[RELEASE] Firmware version: 1.1.3
[SUCCESS] Took ...
[RELEASE] Firmware: .pio/build/esp32doit-devkit-v1/firmware.bin
[RELEASE] Tamanho: ... bytes
[RELEASE] SHA256: ...
[RELEASE] OTA package criado: ...
[RELEASE] Firmware .bin enviado ao ThingsBoard
[RELEASE] Firmware OTA ... vinculado automaticamente ao device ...
[RELEASE] OTA criado, enviado e atribuído ao device com sucesso
```

---

# 23. Problemas comuns

## Device fica ativo, mas não aparece telemetria

Verifique a Rule Chain.

A Root Rule Chain precisa salvar telemetria com `Save Timeseries`.

---

## `Invalid URL '/api/auth/login'`

`TB_URL` está vazio.

Corrija o GitHub Secret:

```txt
TB_URL=https://mytb.seudominio.com
```

---

## `Invalid URL '***/api/auth/login': No scheme supplied`

`TB_URL` está sem `http://` ou `https://`.

Errado:

```txt
mytb.seudominio.com
```

Certo:

```txt
https://mytb.seudominio.com
```

---

## Script não faz nada

Verifique se o arquivo Python termina com:

```python
if __name__ == "__main__":
    main()
```

Sem isso, o Python carrega as funções e encerra sem executar o processo.

---

## Firmware sobe no ThingsBoard, mas ESP não atualiza

Verifique:

```txt
1. O pacote foi vinculado ao device?
2. TB_DEVICE_ID está correto?
3. fw_title do ThingsBoard é igual ao FW_TITLE do firmware?
4. O ESP está conectado ao MQTT?
5. O ESP recebeu atributos compartilhados?
```

---

## Commit não gera release

Commits como `docs:` e `chore:` normalmente não geram versão.

Use:

```txt
fix:
feat:
BREAKING CHANGE:
```

---

# 24. Observação de segurança

Este projeto compila Wi-Fi e token do ThingsBoard dentro do firmware. Para testes isso é aceitável.

Para produção, o ideal é provisionar credenciais por:

```txt
NVS
BLE
Serial
Captive portal
QR Code
Provisionamento por lote
```

Também é recomendado:

```txt
1. Usar um token por device.
2. Rotacionar tokens vazados.
3. Não publicar firmware.bin contendo credenciais.
4. Não expor .env.
5. Usar HTTPS/MQTTS quando possível.
6. Separar profiles de teste, staging e produção.
```

---

# 25. Fluxo final resumido

```txt
1. Criar device no ThingsBoard.
2. Copiar token MQTT.
3. Criar .env local.
4. Subir firmware 1.0.0 via USB.
5. Confirmar telemetria no ThingsBoard.
6. Configurar GitHub Secrets.
7. Fazer commit fix: ou feat:.
8. GitHub Actions calcula versão.
9. PlatformIO compila firmware.bin.
10. Script cria pacote OTA no ThingsBoard.
11. Script vincula pacote ao device.
12. ESP32 baixa firmware em chunks.
13. ESP32 valida checksum.
14. ESP32 reinicia atualizado.
```
