# 训练语料清单（由 tools/mkcorpus.sh 生成，勿手改）

生成时间（UTC）：2026-09-26 09:58:48

## 模型实际训练用的那份

| 文件 | 行数 | 字节 | sha256 |
|---|---|---|---|
| `_corpus.txt` | 21643 | 2165268 | `6c6ceed4e45bf40c` |
| `frames.txt` | 279 | 14078 | `a2ef459fe6e9ffc9` |
| `relations.txt` | 201 | 4276 | `b054ade54a611697` |

## 语料来源与组成

| 文件 | 行数 | 出处 | 行的形态 |
|---|---|---|---|
| `seed_full.txt` | 8088 | 手写模板共现语料 | `[原语:对象] X [原语:分割]` / `[原语:对象] X [原语:对象] Y [原语:连接]` |
| `generated.txt` | 5177 | 纯共现对 | `[原语:对象] A [原语:对象] B [原语:连接]` |
| `dialog.txt` | 8378 | **由 traindialog.c 生成** | `[原语:对象] 用户句 [对话:结束] [原语:对象] 回复句 [对话:结束]` |
| `_corpus.txt` | 21643 | 上三者按 seed+generated+dialog 顺序拼接 | 同上 |
| `frames.txt` | 279 | **由 trainframes.c 生成**（22 个帧） | `[帧:]` / `[问]` / `[答]` / `[降级答]` / `[填:X=hex,…]` / `---` |
| `relations.txt` | 201 | 手写语义关系知识库 | `<主语> <关系词> <宾语>` 与 `<主语> 不 <关系词> <宾语>` |

## 完整语料 all.txt

`all.txt` = `_corpus.txt`（逐字节原样） + 关系事实 + 展开后的帧句子。
边界（行号，从 1 起）：

| 段 | 起 | 止 | 内容 |
|---|---|---|---|
| 1 | 1 | 21643 | `_corpus.txt`（模板共现 + 纯共现 + 一问一答） |
| 2 | 21644 | 21813 | 关系事实（已去掉 # 注释行；含否定事实） |
| 3 | 21814 | 22646 | 帧模板实例化（`tools/mkall.c`，每槽前 3 个填充词） |

合计 **22646** 行，2188525 字节，sha256 `68a2f3e3b3161e19`。

## 怎么用

```sh
# 复现训练 model.bin 的那份语料（= corpus/_corpus.txt）
sh tests/train_model.sh

# 用**完整语料**（含关系事实与帧句子）训一个模型
./build_aip corpus/all.txt --batch --interval 200 --save model_full.bin

# 只重建语料（不改模型）
sh tools/mkcorpus.sh
```

## 注意

- `model.bin` 是用 **`_corpus.txt`（21643 行）** 训出来的，
  `frames.txt` 在训练收尾由 `frames_load()` 当**帧库**读入（不是文本语料），
  `relations.txt` 在读盘时由 `relations_load()` 读入并发成**类型化关系边**。
- 也就是说：`all.txt` 比训练 `model.bin` 用到的**多**了关系事实与帧句子两段。
  想验证「完整语料能训出可用的模型」，用上面第二条命令。

## 可复现性

- `dialog.txt` 由 `traindialog.c` 生成，`frames.txt` 由 `trainframes.c` 生成，
  两者都**逐字节可复现**（重跑生成器再 diff 一致，已验证）。
- `seed_full.txt` 与 `generated.txt` 是手写源，本目录保留原样。
- 因此 `sh tools/mkcorpus.sh` 在任意机器上都会得到同样的 `_corpus.txt` 与 `all.txt`。
