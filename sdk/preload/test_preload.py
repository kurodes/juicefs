#!/usr/bin/env python3
"""Test JuiceFS LD_PRELOAD with AI workload patterns.

Usage:
    export JFS_META_URL="postgres://localhost:5432/mypg?sslmode=disable"
    export JFS_MOUNT_POINT=/jfs
    LD_PRELOAD=./libjfs_preload.so python3 test_preload.py [/jfs]
"""

import hashlib
import json
import os
import pickle
import sys
import time

MOUNT = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("JFS_MOUNT_POINT", "/jfs")
TEST_DIR = os.path.join(MOUNT, f"preload_test_{os.getpid()}")

passed = 0
failed = 0
total_tests = 9


def run_test(num, name, fn):
    global passed, failed
    print(f"\n[{num}/{total_tests}] {name}")
    try:
        fn()
        passed += 1
        print(f"  PASS")
    except Exception as e:
        failed += 1
        print(f"  FAIL — {e}")


# ============================================================================
# Test 1: 基础目录操作（创建实验目录结构）
# ============================================================================
def test_directory_ops():
    dirs = [
        os.path.join(TEST_DIR, "data"),
        os.path.join(TEST_DIR, "data", "train"),
        os.path.join(TEST_DIR, "data", "val"),
        os.path.join(TEST_DIR, "checkpoints"),
        os.path.join(TEST_DIR, "logs"),
    ]
    for d in dirs:
        os.makedirs(d, exist_ok=True)
        print(f"  makedirs {d}")

    entries = sorted(os.listdir(TEST_DIR))
    print(f"  listdir {TEST_DIR} → {entries}")
    assert set(entries) == {"data", "checkpoints", "logs"}, f"unexpected entries: {entries}"

    assert os.path.exists(TEST_DIR), f"{TEST_DIR} should exist"
    print(f"  exists {TEST_DIR} → True")

    assert os.path.isdir(os.path.join(TEST_DIR, "data")), "data should be a directory"
    print(f"  isdir {TEST_DIR}/data → True")

    sub_entries = sorted(os.listdir(os.path.join(TEST_DIR, "data")))
    print(f"  listdir {TEST_DIR}/data → {sub_entries}")
    assert sub_entries == ["train", "val"], f"unexpected sub entries: {sub_entries}"


# ============================================================================
# Test 2: 文本文件读写（配置文件、日志）
# ============================================================================
def test_text_readwrite():
    filepath = os.path.join(TEST_DIR, "config.yaml")

    content = "learning_rate: 0.001\nbatch_size: 32\nepochs: 100\n"
    with open(filepath, "w") as f:
        f.write(content)
    print(f"  write {filepath} ({len(content)} bytes)")

    with open(filepath, "r") as f:
        read_back = f.read()
    match = read_back == content
    print(f"  read  {filepath} → {len(read_back)} bytes, content matches: {match}")
    assert match, "text content mismatch"

    append_text = "optimizer: adam\n"
    with open(filepath, "a") as f:
        f.write(append_text)
    print(f"  append {filepath} (+{len(append_text)} bytes)")

    with open(filepath, "r") as f:
        full = f.read()
    expected_len = len(content) + len(append_text)
    print(f"  read  {filepath} → {len(full)} bytes (expected {expected_len})")
    assert len(full) == expected_len, f"append failed: got {len(full)} bytes"
    assert full == content + append_text, "appended content mismatch"


# ============================================================================
# Test 3: 二进制大文件读写（模型 checkpoint）
# ============================================================================
def test_large_binary_file():
    filepath = os.path.join(TEST_DIR, "checkpoints", "model_epoch10.bin")
    size = 10 * 1024 * 1024  # 10 MB

    # 生成可重复的伪随机数据
    data = os.urandom(size)
    expected_md5 = hashlib.md5(data).hexdigest()

    with open(filepath, "wb") as f:
        f.write(data)
    print(f"  write {filepath} ({size} bytes)")

    file_size = os.path.getsize(filepath)
    print(f"  getsize → {file_size}")
    assert file_size == size, f"size mismatch: {file_size} != {size}"

    with open(filepath, "rb") as f:
        read_back = f.read()
    actual_md5 = hashlib.md5(read_back).hexdigest()
    match = actual_md5 == expected_md5
    print(f"  read + md5 verify → expected={expected_md5}, actual={actual_md5}, match: {match}")
    assert match, "large file MD5 mismatch"


# ============================================================================
# Test 4: 批量小文件（数据集 shards）
# ============================================================================
def test_batch_small_files():
    shard_dir = os.path.join(TEST_DIR, "data", "train")
    num_files = 100

    for i in range(num_files):
        path = os.path.join(shard_dir, f"shard_{i:04d}.bin")
        with open(path, "wb") as f:
            f.write(f"data_for_shard_{i}".encode())
    print(f"  created {num_files} shard files in {shard_dir}")

    entries = os.listdir(shard_dir)
    print(f"  listdir → {len(entries)} files")
    assert len(entries) == num_files, f"expected {num_files}, got {len(entries)}"

    # 验证部分文件内容
    for i in [0, 49, 99]:
        path = os.path.join(shard_dir, f"shard_{i:04d}.bin")
        with open(path, "rb") as f:
            content = f.read()
        expected = f"data_for_shard_{i}".encode()
        assert content == expected, f"shard {i} content mismatch"
    print(f"  verified shard 0, 49, 99 content: OK")


# ============================================================================
# Test 5: 文件元数据（stat, chmod, utime）
# ============================================================================
def test_file_metadata():
    filepath = os.path.join(TEST_DIR, "metadata_test.txt")
    with open(filepath, "w") as f:
        f.write("metadata test\n")
    print(f"  created {filepath}")

    st = os.stat(filepath)
    print(f"  stat → size={st.st_size}, mode={oct(st.st_mode)}, mtime={st.st_mtime}")
    assert st.st_size == 14, f"unexpected size: {st.st_size}"

    os.chmod(filepath, 0o755)
    st2 = os.stat(filepath)
    print(f"  chmod 755 → mode={oct(st2.st_mode)}")
    assert st2.st_mode & 0o777 == 0o755, f"chmod failed: {oct(st2.st_mode)}"

    target_time = 1700000000.0  # 固定时间戳
    os.utime(filepath, (target_time, target_time))
    st3 = os.stat(filepath)
    print(f"  utime → atime={st3.st_atime}, mtime={st3.st_mtime}")
    assert abs(st3.st_mtime - target_time) < 2, f"utime failed: {st3.st_mtime}"


# ============================================================================
# Test 6: 文件管理（rename, symlink, remove）
# ============================================================================
def test_file_management():
    src = os.path.join(TEST_DIR, "src_file.txt")
    dst = os.path.join(TEST_DIR, "dst_file.txt")

    with open(src, "w") as f:
        f.write("rename me\n")
    print(f"  created {src}")

    os.rename(src, dst)
    print(f"  rename {src} → {dst}")
    assert os.path.exists(dst), "dst should exist after rename"
    assert not os.path.exists(src), "src should not exist after rename"

    with open(dst, "r") as f:
        assert f.read() == "rename me\n", "content changed after rename"
    print(f"  verified content after rename: OK")

    link_path = os.path.join(TEST_DIR, "link_to_dst")
    os.symlink(dst, link_path)
    print(f"  symlink {link_path} → {dst}")

    target = os.readlink(link_path)
    print(f"  readlink {link_path} → {target}")
    assert target == dst, f"readlink mismatch: {target}"

    os.remove(link_path)
    print(f"  remove {link_path}")
    assert not os.path.exists(link_path), "symlink should be removed"

    os.unlink(dst)
    print(f"  unlink {dst}")
    assert not os.path.exists(dst), "dst should be removed"


# ============================================================================
# Test 7: JSON 配置读写（训练超参数）
# ============================================================================
def test_json_config():
    filepath = os.path.join(TEST_DIR, "hparams.json")
    config = {
        "model": "resnet50",
        "learning_rate": 0.001,
        "batch_size": 64,
        "epochs": 200,
        "optimizer": {"name": "adam", "beta1": 0.9, "beta2": 0.999},
        "data_augmentation": ["flip", "rotate", "crop"],
    }

    with open(filepath, "w") as f:
        json.dump(config, f, indent=2)
    print(f"  json.dump → {filepath}")

    with open(filepath, "r") as f:
        loaded = json.load(f)
    print(f"  json.load → keys={list(loaded.keys())}")
    assert loaded == config, "JSON round-trip mismatch"
    print(f"  round-trip verify: OK")


# ============================================================================
# Test 8: Pickle 序列化（模拟 PyTorch checkpoint）
# ============================================================================
def test_pickle_checkpoint():
    filepath = os.path.join(TEST_DIR, "checkpoints", "ckpt_epoch5.pkl")
    checkpoint = {
        "epoch": 5,
        "model_state": {f"layer{i}.weight": list(range(100)) for i in range(10)},
        "optimizer_state": {"lr": 0.001, "step": 5000},
        "loss": 0.0234,
        "metrics": {"accuracy": 0.956, "f1": 0.943},
    }

    with open(filepath, "wb") as f:
        pickle.dump(checkpoint, f)
    size = os.path.getsize(filepath)
    print(f"  pickle.dump → {filepath} ({size} bytes)")

    with open(filepath, "rb") as f:
        loaded = pickle.load(f)
    print(f"  pickle.load → epoch={loaded['epoch']}, loss={loaded['loss']}")
    assert loaded["epoch"] == checkpoint["epoch"], "epoch mismatch"
    assert loaded["loss"] == checkpoint["loss"], "loss mismatch"
    assert loaded["model_state"] == checkpoint["model_state"], "model_state mismatch"
    print(f"  round-trip verify: OK")


# ============================================================================
# Test 9: seek 操作（随机读取）
# ============================================================================
def test_seek_operations():
    filepath = os.path.join(TEST_DIR, "seek_test.bin")

    # 写入已知 pattern: 0x00*1024 + 0xFF*1024 + 0xAB*1024
    with open(filepath, "wb") as f:
        f.write(b"\x00" * 1024)
        f.write(b"\xff" * 1024)
        f.write(b"\xab" * 1024)
    print(f"  write {filepath} (3072 bytes, 3 blocks)")

    with open(filepath, "rb") as f:
        # seek 到第二块
        f.seek(1024)
        pos = f.tell()
        print(f"  seek(1024) → tell()={pos}")
        assert pos == 1024, f"tell mismatch: {pos}"

        chunk = f.read(16)
        print(f"  read(16) → {chunk.hex()}")
        assert chunk == b"\xff" * 16, "seek+read content mismatch at offset 1024"

        # seek 到第三块
        f.seek(2048)
        chunk = f.read(16)
        print(f"  seek(2048) + read(16) → {chunk.hex()}")
        assert chunk == b"\xab" * 16, "seek+read content mismatch at offset 2048"

        # seek from end
        f.seek(-512, 2)  # SEEK_END
        pos = f.tell()
        print(f"  seek(-512, SEEK_END) → tell()={pos}")
        assert pos == 3072 - 512, f"seek from end failed: {pos}"


# ============================================================================
# Main
# ============================================================================
def main():
    print(f"JuiceFS LD_PRELOAD Test Suite")
    print(f"  mount point: {MOUNT}")
    print(f"  test dir:    {TEST_DIR}")
    print(f"  pid:         {os.getpid()}")

    run_test(1, "基础目录操作", test_directory_ops)
    run_test(2, "文本文件读写", test_text_readwrite)
    run_test(3, "大文件读写 (10MB checkpoint)", test_large_binary_file)
    run_test(4, "批量小文件 (100 shards)", test_batch_small_files)
    run_test(5, "文件元数据 (stat/chmod/utime)", test_file_metadata)
    run_test(6, "文件管理 (rename/symlink/remove)", test_file_management)
    run_test(7, "JSON 配置读写", test_json_config)
    run_test(8, "Pickle checkpoint 读写", test_pickle_checkpoint)
    run_test(9, "Seek 随机读取", test_seek_operations)

    print(f"\n{'='*50}")
    print(f"Results: {passed} passed, {failed} failed (total {total_tests})")
    print(f"{'='*50}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
