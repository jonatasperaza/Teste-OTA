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
    print(f"[CMD] {' '.join(cmd)}")
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
        "deviceProfileId": {"entityType": "DEVICE_PROFILE", "id": device_profile_id},
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

    return response.json()["id"]["id"]


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
