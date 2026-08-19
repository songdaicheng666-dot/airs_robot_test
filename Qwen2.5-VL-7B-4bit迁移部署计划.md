# Qwen2.5-VL-7B 4bit 迁移部署计划

> 给目标电脑 Codex 的执行指令：请在目标 Ubuntu 22.04 电脑上实际完成本文全部步骤，而不只是给出建议。先执行只读预检，预检通过后再创建运行环境和脚本。遇到本文定义的停止条件时保留完整诊断信息并报告，不要擅自修改权重、重装显卡驱动或扩大服务暴露范围。

## 1. 部署目标和边界

将已复制到目标电脑的 `7B_4bit` 目录部署为手动启停的本地 vLLM 服务：

- 模型：Qwen2.5-VL-7B-Instruct，bitsandbytes NF4 4bit。
- API：`http://127.0.0.1:8001/v1`。
- 服务模型名：`qwen2.5-vl-7b-bnb-4bit`。
- 首版配置：BF16 计算、16K 上下文、单并发、约 85% GPU 显存上限。
- 硬件目标：Ubuntu 22.04、RTX 3090 或 RTX A5000、24GB 显存。
- 使用独立 Python 虚拟环境，不修改系统 Python 包。
- 不配置 systemd 和开机自启动；不开放局域网或公网端口。
- 本轮只部署和验收 VLM，不连接 ECS，不下发机器人命令，不读取或保存任何 ECS Token。

后续 PC 编排程序应分别使用：

```text
VLM_BASE_URL=http://127.0.0.1:8001/v1
VLM_MODEL=qwen2.5-vl-7b-bnb-4bit
RELAY_BASE_URL=<ECS HTTPS 地址>
```

VLM 进程只负责推理。后续应由独立的 PC 编排层完成“模型回答 -> 严格 JSON Schema 校验 -> 命令白名单和安全检查 -> ECS”，VLM 进程本身不得持有 `RELAY_OPERATOR_TOKEN`。

## 2. 预量化模型的加载原则

`load_in_4bit` 是把全精度模型在加载时现场量化为 4bit 的开关。当前目录中的权重已经完成量化，`config.json` 自带类似以下配置：

```json
{
  "quantization_config": {
    "quant_method": "bitsandbytes",
    "load_in_4bit": true,
    "bnb_4bit_quant_type": "nf4",
    "bnb_4bit_use_double_quant": true,
    "bnb_4bit_compute_dtype": "bfloat16"
  }
}
```

因此部署时应直接把本地模型目录交给 vLLM，让它读取检查点中的 `quantization_config`。不要在外部再次传入 Transformers 的 `load_in_4bit=True`，也不要重新量化，否则可能触发重复量化保护或配置冲突。

启动参数中的 `--dtype bfloat16` 不会把量化权重恢复为 BF16。它主要控制推理计算类型，以及视觉模块、多模态 projector 和 `lm_head` 等未量化模块的类型。若 vLLM 没有自动识别量化格式，可按本文故障处理步骤额外尝试 `--quantization bitsandbytes`；该参数用于明确选择加载器，不是重新量化。

## 3. 只读预检

### 3.1 定位模型

不要假设用户把目录复制到了固定绝对路径。搜索并确认只存在一个：

```text
7B_4bit/7B/Qwen2.5-VL-7B-Instruct-bnb-4bit
```

若没有找到或找到多个候选目录，停止并让用户确认。不要使用目录中现有的 `serve_qwen25vl_7b_bnb4bit_vllm.sh`：该脚本仍会跳转到原工程根目录并依赖未随模型复制的 `scripts/validate_vllm_checkpoint.py`，不是自包含入口。

### 3.2 检查硬件和系统

执行并记录：

```bash
cat /etc/os-release
uname -a
nvidia-smi
nvidia-smi --query-gpu=name,driver_version,memory.total,memory.used --format=csv,noheader
python3 --version
df -h .
free -h
```

继续部署必须满足：

- `nvidia-smi` 正常，识别到 RTX 3090 或 RTX A5000 和约 24GB 显存。
- Python 为 3.10；若仅缺少 `python3-venv`，可在获得用户批准后通过 APT 安装。
- 模型目录所在磁盘至少还有 25GB 可用空间。
- 没有其他进程占用大量显存。只能报告进程信息，不得擅自结束其他进程。

若显卡驱动不可用，停止并报告，不自动安装或更换驱动，也不自动重启电脑。

### 3.3 校验模型完整性

模型目录应包含 15 个普通文件，文件总大小应为 `6917042721` 字节。计算并核对以下 SHA-256：

```text
d2d6d1ab63bd22d895697c16f4e579b62bb198bce44be5575ac941a370c0d5f4  model-00001-of-00002.safetensors
a84cabffbc7fdb23d3e72598249bc3d7053a060c609fab106b92484423b22cf9  model-00002-of-00002.safetensors
db9ffa3c02bc1f684c077020bdf86f7f6a7ae965369be2f2ff2da57a8a90b73a  model.safetensors.index.json
d5820df31f70230e7ca25414fcaf7dae618b2c1ab7ba4e4da0f5a213132ae0d4  config.json
20a2881aaeaaf29a37e08ac9f9b9f90350ee2ec0c1fe99ad22d83d7551be23d3  quantization_manifest.json
```

解析 JSON 并验证：

- `config.json` 的 `model_type` 为 `qwen2_5_vl`。
- `architectures` 包含 `Qwen2_5_VLForConditionalGeneration`。
- `quantization_config.quant_method` 为 `bitsandbytes`。
- `quantization_manifest.json` 的 `format` 为 `bitsandbytes-4bit`、`profile` 为 `qwen25vl-7b`、`vllm_compatible` 为 `true`。
- `model.safetensors.index.json` 引用的分片都真实存在。

任何文件数量、大小、散列或配置不一致时立即停止。不要尝试修补 JSON 或重新量化。

## 4. 创建隔离运行环境

在 `7B_4bit` 下创建 `runtime/`，所有新增部署文件都放在此目录，模型目录保持只读和不变：

```text
7B_4bit/
├── 7B/
│   └── Qwen2.5-VL-7B-Instruct-bnb-4bit/
└── runtime/
    ├── .venv/
    ├── run_vllm.sh
    ├── smoke_test.py
    ├── README.md
    ├── requirements.lock.txt
    └── environment-report.txt
```

使用 Python 3.10 创建 `runtime/.venv`，升级虚拟环境中的 `pip`、`setuptools` 和 `wheel`，然后联网安装：

```text
vllm
bitsandbytes
pillow
requests
```

让 pip 为 vLLM 解析匹配的 Torch、Transformers 和 CUDA runtime wheel。不要额外安装系统 CUDA Toolkit，也不要先单独固定一个不确定兼容性的 Torch 版本。

安装后执行：

```bash
python -m pip check
```

再通过 Python 验证并记录：

- `torch.cuda.is_available()` 为 `True`。
- GPU compute capability 不低于 8.0。
- `torch.cuda.is_bf16_supported()` 为 `True`。
- 能正常导入 `vllm`、`transformers` 和 `bitsandbytes`。

将最终 `pip freeze` 写入 `runtime/requirements.lock.txt`。将操作系统、Python、显卡、驱动、Torch、Torch CUDA、vLLM、Transformers 和 bitsandbytes 版本写入 `runtime/environment-report.txt`，不得在报告中写入任何 Token。

## 5. 创建自包含启动脚本

创建可执行的 `runtime/run_vllm.sh`。脚本必须根据自身位置推导虚拟环境和模型路径，不得包含目标电脑用户名或绝对路径。

脚本应具有以下行为：

- 使用 `set -euo pipefail`。
- 固定 `CUDA_VISIBLE_DEVICES=0`、`VLLM_NO_USAGE_STATS=1`。
- 设置 `HF_HUB_OFFLINE=1` 和 `TRANSFORMERS_OFFLINE=1`，确保启动时只读取本地模型。
- 固定监听 `127.0.0.1`，不允许通过环境变量改成 `0.0.0.0`。
- 默认端口为 `8001`。
- 默认 `MAX_MODEL_LEN=16384`，允许通过同名环境变量降低。
- 默认 `GPU_MEMORY_UTILIZATION=0.85`，允许通过同名环境变量调整。
- 使用虚拟环境内的 `vllm` 可执行文件，不依赖调用者是否已经 `source activate`。

实际服务参数应等价于：

```bash
vllm serve "<本地模型目录>" \
  --host 127.0.0.1 \
  --port 8001 \
  --served-model-name qwen2.5-vl-7b-bnb-4bit \
  --trust-remote-code \
  --dtype bfloat16 \
  --gpu-memory-utilization 0.85 \
  --max-model-len 16384 \
  --max-num-seqs 1
```

不要传入 `load_in_4bit`，不要设置 ECS Token，不要添加 systemd、Nginx 或防火墙规则。

## 6. 创建并执行冒烟测试

创建 `runtime/smoke_test.py`，使用 `requests` 和 Pillow 完成以下检查：

1. 最多等待 10 分钟，直到 `GET http://127.0.0.1:8001/health` 成功。
2. 请求 `GET /v1/models`，确认返回 `qwen2.5-vl-7b-bnb-4bit`。
3. 向 `POST /v1/chat/completions` 发送一个确定性的文本请求，要求返回简短内容；验证 HTTP 200、存在 `choices[0].message.content` 且内容非空。
4. 使用 Pillow 在内存中生成一张纯红色测试图，编码为 data URL，以 OpenAI 图文消息格式发送。
5. 图片提示要求模型仅用一个英文单词回答主色；验证响应非空且忽略大小写后包含 `red`。
6. 所有请求设置合理超时，失败时打印 HTTP 状态和响应正文，但不泄露环境变量。

执行流程：

1. 在一个终端以前台方式运行 `runtime/run_vllm.sh`。
2. 等待 vLLM 完成权重加载。
3. 在另一个终端运行 `runtime/smoke_test.py`。
4. 同时检查 `nvidia-smi`，记录模型加载后的显存占用。
5. 使用 `ss -ltnp` 确认服务只监听 `127.0.0.1:8001`，不得出现 `0.0.0.0:8001`。
6. 验收完成后用 `Ctrl+C` 停止测试实例，确认端口释放。

在 `runtime/README.md` 中记录最终启动、测试、查看显存和停止命令，以及本地 API 地址和模型名。

## 7. 故障处理顺序

### 7.1 显存不足

先确认不存在其他大量占用显存的进程，不得擅自终止它们。若 GPU 基本空闲仍然 OOM，按以下顺序各重试一次：

```bash
MAX_MODEL_LEN=8192 runtime/run_vllm.sh
```

若日志明确显示可用于 KV cache 的空间不足，再重试：

```bash
MAX_MODEL_LEN=8192 GPU_MEMORY_UTILIZATION=0.90 runtime/run_vllm.sh
```

仍失败则停止并报告，不启用 CPU offload，不切换成 CPU 推理。

### 7.2 未识别量化格式

若 vLLM 日志明确表明没有自动识别 bitsandbytes 检查点，只额外尝试一次在 vLLM 命令中加入：

```text
--quantization bitsandbytes
```

仍失败时保留完整启动日志、`environment-report.txt` 和 `pip check` 输出并停止。不要重新量化、下载同名模型覆盖现有目录或修改 `config.json`。

### 7.3 其他停止条件

遇到以下任一情况都应停止并报告：

- 权重散列不一致。
- BF16 支持检查失败。
- NVIDIA 驱动或 CUDA 在 Torch 中不可用。
- pip 依赖无法得到一致环境。
- 文本可以推理但图片请求导致服务崩溃。
- 服务只能通过监听 `0.0.0.0` 才能工作。

不得静默降级为 FP16、CPU offload、远程模型下载或对外开放端口。

## 8. 最终验收标准

只有同时满足以下条件才算部署成功：

- 模型完整性和量化配置校验全部通过。
- 虚拟环境依赖通过 `pip check`，环境版本已经锁定和记录。
- vLLM 成功在 RTX 3090/A5000 上加载模型，运行期间没有持续 OOM。
- `/health`、`/v1/models`、文本推理和图片推理全部通过。
- 服务仅监听 `127.0.0.1:8001`。
- 没有访问 ECS、没有发送机器人命令、没有读取或保存 ECS Token。
- 没有配置开机自启动，测试结束后服务处于停止状态。

最终向用户报告：

- 模型目录和启动脚本的实际路径。
- 手动启动命令。
- API 地址和模型名。
- 实际安装的关键依赖版本。
- 模型加载后的显存占用。
- 文本和图片冒烟测试结果。
- 若使用了 8K 降级参数，明确说明原因；不得把降级结果伪装成默认 16K 验收通过。
