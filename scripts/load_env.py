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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)

    args = parser.parse_args()

    version = args.version

    tb_url = os.environ["TB_URL"]
    tb_username = os.environ["TB_USERNAME"]
    tb_password = os.environ["TB_PASSWORD"]
    tb_device_profile_id = os.environ["TB_DEVICE_PROFILE_ID"]

    fw_title = os.getenv("FW_TITLE", "esp32-tb-ota-test")

    build_env = os.environ.copy()
    build_env["FW_VERSION"] = version
    build_env["FW_TITLE"] = fw_title

    print(f"[RELEASE] Firmware title: {fw_title}")
    print(f"[RELEASE] Firmware version: {version}")

    run(["pio", "run", "-e", ENV_NAME], env=build_env)

    if not FIRMWARE_PATH.exists():
        raise RuntimeError(f"Firmware não encontrado: {FIRMWARE_PATH}")

    checksum = sha256_file(FIRMWARE_PATH)

    print(f"[RELEASE] Firmware: {FIRMWARE_PATH}")
    print(f"[RELEASE] SHA256: {checksum}")

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

    print("[RELEASE] OTA enviado para o ThingsBoard com sucesso")


if __name__ == "__main__":
    main()
