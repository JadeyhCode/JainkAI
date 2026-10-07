# 语料完整性验证记录

记录时间（UTC）：2026-09-26 09:58:48

## 1. 生成器可复现（逐字节）

```
$ gcc -O2 -o traindialog traindialog.c && ./traindialog _chk.txt && diff -q _chk.txt dialog.txt
  已生成 8378 行对话语料
  （diff 无输出 = 逐字节一致）

$ gcc -O2 -o trainframes trainframes.c && ./trainframes _fr.txt && diff -q _fr.txt frames.txt
  已生成 22 个帧
  （diff 无输出 = 逐字节一致）
```

## 2. 完整语料能训出可用的模型

```
$ ./build_aip corpus/all.txt --batch --interval 200 --save model_full.bin
  Token=1214 (活跃1115 淡化99)  边=42352  已学=247217  轮次=22600
  （对照：model.bin = 用 _corpus.txt 训的  Token=1195  边=41134  轮次=21600）
```

## 3. 用完整语料训出来的模型，推理链照样成立

```
$ ./build_aip   # 加载 model_full.bin
你 > 猫 有 毛 吗
AI > 对 ， 猫 -[属于·继承]-> 动物 -[有]-> 毛
你 > 猫 会 飞 吗
AI > 不 对 ， 猫 -[不 会]-> 飞 （推得出不是）
你 > 猫 会 唱歌 吗
AI > 这个我 不 太 懂          ← 三态里的「不知道」，不装懂也不硬说不对
你 > 你好呀
AI > 晚上 好 早 点 休息
```

## 4. 回归

```
$ sh tests/all.sh
  PASS model_roundtrip / features / frames(27/28) / framegate_ab
  PASS coining / segmentation / cot / embed / relations
  全部回归通过
```
