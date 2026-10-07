cmake_minimum_required(VERSION 3.22)
project(AIPoject C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 主智能体
add_executable(AIPoject main.c)
target_link_libraries(AIPoject m)
target_compile_options(AIPoject PRIVATE -Wall -Wextra)

# 语料生成器
add_executable(train train.c)
target_compile_options(train PRIVATE -Wall -Wextra)

# 微内核自测（C 级，不经过 CLI）：队列/预算/每条拒绝理由/检测器/补丁故障隔离/元认知
add_executable(microtest tools/microtest.c)
target_link_libraries(microtest m)
target_compile_options(microtest PRIVATE -Wall -Wextra)

# 思维循环自测（C 级）：议程/反方向/矛盾/跨轮/铁律/元认知
add_executable(delibtest tools/delibtest.c)
target_link_libraries(delibtest m)
target_compile_options(delibtest PRIVATE -Wall -Wextra)

# 共现固化判据的实测入口（看某条边为什么会被/不会被固化）
add_executable(overlapprobe tools/overlapprobe.c)
target_link_libraries(overlapprobe m)
target_compile_options(overlapprobe PRIVATE -Wall -Wextra)

# 清单自检（C 级）：疑问词/虚词/助词/关系表/规则表没有重复项，两张 frame 词表不分家
add_executable(dupcheck tools/dupcheck.c)
target_link_libraries(dupcheck m)
target_compile_options(dupcheck PRIVATE -Wall -Wextra)

# 改口语义的存量测量：模型里有多少条「正负极性并存」的边（改这条语义前先量）
add_executable(revprobe tools/revprobe.c)
target_link_libraries(revprobe m)
target_compile_options(revprobe PRIVATE -Wall -Wextra)

# 反糟粕自测（C 级）：一次巧合不是规律 / 反事实真削弱 / 不虚构 / 不自我印证 / 不空想造事实 / 不从众 / 不泛化
add_executable(biastest tools/biastest.c)
target_link_libraries(biastest m)
target_compile_options(biastest PRIVATE -Wall -Wextra)

# 因果猜想生命周期的实测入口：一次=暂时 / 协变=升格 / 反例=削弱（改这条语义前先跑它）
add_executable(causalprobe tools/causalprobe.c)
target_link_libraries(causalprobe m)
target_compile_options(causalprobe PRIVATE -Wall -Wextra)

# Token 行为（图操作）/ 临时 Token / 修正的记忆自测（C 级）
add_executable(optest tools/optest.c)
target_link_libraries(optest m)
target_compile_options(optest PRIVATE -Wall -Wextra)
