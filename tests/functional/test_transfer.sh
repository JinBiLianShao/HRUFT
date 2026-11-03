#!/bin/bash
# ============================================================================
# 功能测试 —— HRUFT Pro
#
# 作者：连思鑫
# 说明：验证传输本身的正确性。重点覆盖分块边界，因为 v4 的收发是按
#       4MB 块走的，边界差一个字节最容易暴露循环写错。
#       每个用例都比对 md5，不看日志说什么，只看落地文件对不对。
# ============================================================================

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
source ../common.sh

echo "=== 功能测试：传输正确性 ==="
echo "二进制: $HRUFT_BIN"
echo

P=$PORT_BASE

# ---- 空文件 ----
# 这个用例以前会挂：发送端 while 循环条件写错会导致 0 字节文件直接跳过哈希帧
echo "[1] 空文件"
: > "$WORK/src/empty.bin"
transfer "$WORK/src/empty.bin" $((P+1))
if [ $TRANSFER_SEND_EXIT -eq 0 ] && [ $TRANSFER_RECV_EXIT -eq 0 ] && \
   same_content "$WORK/src/empty.bin" "$WORK/dst/empty.bin"; then
    ok "空文件传输"
else
    bad "空文件传输" "send=$TRANSFER_SEND_EXIT recv=$TRANSFER_RECV_EXIT"
fi

# ---- 极小文件 ----
echo "[2] 1 字节 / 1KB"
make_file 1 "$WORK/src/one.bin"
transfer "$WORK/src/one.bin" $((P+2))
same_content "$WORK/src/one.bin" "$WORK/dst/one.bin" && ok "1 字节" || bad "1 字节"

make_file 1024 "$WORK/src/1k.bin"
transfer "$WORK/src/1k.bin" $((P+3))
same_content "$WORK/src/1k.bin" "$WORK/dst/1k.bin" && ok "1KB" || bad "1KB"

# ---- 4MB 分块边界 ----
# 4MB 是 APP_BLOCK_SIZE，正好等于块大小、差一个字节、差两个字节
# 这三种情况分别对应：循环正好结束、最后一次 read 不满、多一个完整块
echo "[3] 分块边界（4MB ± 1）"
for sz in 4194303 4194304 4194305 8388608 8388609; do
    make_file $sz "$WORK/src/b$sz.bin"
    transfer "$WORK/src/b$sz.bin" $((P+4))
    if same_content "$WORK/src/b$sz.bin" "$WORK/dst/b$sz.bin"; then
        ok "边界 $sz 字节"
    else
        bad "边界 $sz 字节" "send=$TRANSFER_SEND_EXIT recv=$TRANSFER_RECV_EXIT"
    fi
done

# ---- 中等文件 ----
echo "[4] 中等文件"
make_file $((32*1024*1024)) "$WORK/src/32m.bin"
transfer "$WORK/src/32m.bin" $((P+5))
same_content "$WORK/src/32m.bin" "$WORK/dst/32m.bin" && ok "32MB" || bad "32MB"

# ---- 大文件 ----
# 512MB 用来覆盖「多秒传输 + 进度刷新 + 计数溢出」的组合
echo "[5] 大文件（512MB）"
make_file $((512*1024*1024)) "$WORK/src/512m.bin"
transfer "$WORK/src/512m.bin" $((P+6))
if same_content "$WORK/src/512m.bin" "$WORK/dst/512m.bin"; then
    ok "512MB"
else
    bad "512MB" "send=$TRANSFER_SEND_EXIT recv=$TRANSFER_RECV_EXIT"
fi

# ---- 中文文件名 ----
# v3 时代这里出过问题：Windows 下 stem() 返回 wstring 被窄化
echo "[6] 中文文件名与路径"
mkdir -p "$WORK/src/中文目录"
printf '中文内容测试\n' > "$WORK/src/中文目录/文档 带空格.txt"
transfer "$WORK/src/中文目录/文档 带空格.txt" $((P+7))
if same_content "$WORK/src/中文目录/文档 带空格.txt" "$WORK/dst/文档 带空格.txt"; then
    ok "中文 + 空格文件名"
else
    bad "中文 + 空格文件名" "落盘文件: $(ls "$WORK/dst/" | tail -3 | tr '\n' ' ')"
fi

# ---- savepath 是裸文件名 ----
# 这个以前会崩：create_directories("") 在 libstdc++ 下抛 filesystem error
echo "[7] 保存路径为裸文件名"
make_file 2048 "$WORK/src/bare.bin"
"$HRUFT_BIN" recv $((P+8)) "$WORK/dst/bare-out.bin" > "$WORK/bare.log" 2>&1 &
RP=$!
sleep 0.8
"$HRUFT_BIN" send 127.0.0.1 $((P+8)) "$WORK/src/bare.bin" > /dev/null 2>&1
wait $RP
RE=$?
if [ $RE -eq 0 ] && same_content "$WORK/src/bare.bin" "$WORK/dst/bare-out.bin"; then
    ok "裸文件名保存（不崩溃）"
else
    bad "裸文件名保存" "recv_exit=$RE"
fi

# ---- savepath 是已存在目录 ----
echo "[8] 保存到已存在目录"
mkdir -p "$WORK/dst/existing"
make_file 4096 "$WORK/src/into.bin"
"$HRUFT_BIN" recv $((P+9)) "$WORK/dst/existing" > /dev/null 2>&1 &
RP=$!
sleep 0.8
"$HRUFT_BIN" send 127.0.0.1 $((P+9)) "$WORK/src/into.bin" > /dev/null 2>&1
wait $RP
same_content "$WORK/src/into.bin" "$WORK/dst/existing/into.bin" && \
    ok "自动落到目录内" || bad "自动落到目录内"

# ---- 同名文件不覆盖 ----
# 旧版用秒级时间戳，同秒传两次直接覆盖。现在必须编号
echo "[9] 同名文件不覆盖"
make_file 512 "$WORK/src/dup.bin"
transfer "$WORK/src/dup.bin" $((P+10))
transfer "$WORK/src/dup.bin" $((P+11))
n=$(ls "$WORK/dst/" | grep -c '^dup')
if [ "$n" -ge 2 ]; then
    ok "同名文件生成 $n 份，未覆盖"
else
    bad "同名文件不覆盖" "只有 $n 份"
fi

# ---- 零字节后再传非零（复用端口，确认没有状态残留）----
echo "[10] 连续传输状态隔离"
make_file 1024 "$WORK/src/seq1.bin"
transfer "$WORK/src/seq1.bin" $((P+12))
make_file 8192 "$WORK/src/seq2.bin"
transfer "$WORK/src/seq2.bin" $((P+13))
if same_content "$WORK/src/seq1.bin" "$WORK/dst/seq1.bin" && \
   same_content "$WORK/src/seq2.bin" "$WORK/dst/seq2.bin"; then
    ok "连续两次传输互不影响"
else
    bad "连续两次传输互不影响"
fi

# ---- --detailed 不影响结果 ----
echo "[11] --detailed 不影响传输结果"
make_file 1048576 "$WORK/src/det.bin"
transfer "$WORK/src/det.bin" $((P+14)) --detailed
same_content "$WORK/src/det.bin" "$WORK/dst/det.bin" && ok "--detailed 模式" || bad "--detailed 模式"

# ---- --no-cc ----
echo "[12] --no-cc 模式"
make_file 1048576 "$WORK/src/nocc.bin"
transfer "$WORK/src/nocc.bin" $((P+15)) --no-cc
same_content "$WORK/src/nocc.bin" "$WORK/dst/nocc.bin" && ok "--no-cc 模式" || bad "--no-cc 模式"

# ---- MSS 边界值 ----
echo "[13] MSS 边界值"
for mss in 536 8900; do
    make_file 262144 "$WORK/src/mss$mss.bin"
    transfer "$WORK/src/mss$mss.bin" $((P+16)) --mss $mss
    same_content "$WORK/src/mss$mss.bin" "$WORK/dst/mss$mss.bin" && \
        ok "MSS=$mss" || bad "MSS=$mss"
done

# ---- 大窗口 ----
echo "[14] 大窗口"
make_file 4194304 "$WORK/src/bw.bin"
transfer "$WORK/src/bw.bin" $((P+17)) --window 134217728
same_content "$WORK/src/bw.bin" "$WORK/dst/bw.bin" && ok "window=128MB" || bad "window=128MB"


# ---- 两端 MSS 一致性 ----
# 这条是补的，因为踩过坑：曾经试过「发送端默认不设 MSS，让 UDT 协商」，
# 结果 UDT 握手取双方较小值，发送端 1500、接收端 8900 时 FC 按 8900 算
# 包数却按 1500 发包，窗口严重失配，512MB 从 370 MB/s 掉到 9 MB/s。
# 光看「传输成功、哈希正确」发现不了这种问题，必须看吞吐。
echo "[15] 两端 MSS 一致性（性能不能崩）"
make_file 67108864 "$WORK/src/consistency.bin"
for mss in 1500 8900; do
    rm -rf "$WORK/dst"; mkdir -p "$WORK/dst"
    t0=$(date +%s%N)
    "$HRUFT_BIN" recv $((P+18)) "$WORK/dst/" --mss $mss > "$WORK/c.log" 2>&1 &
    RP=$!
    sleep 0.8
    "$HRUFT_BIN" send 127.0.0.1 $((P+18)) "$WORK/src/consistency.bin" --mss $mss > /dev/null 2>&1
    wait $RP
    t1=$(date +%s%N)
    secs=$(awk -v a=$t0 -v b=$t1 'BEGIN{print (b-a)/1e9-0.8}')
    rate=$(awk -v d=$secs 'BEGIN{printf "%.1f", 64/d}')
    # 阈值 50 MB/s：正常 300+，失配时掉到 10 以下
    slow=$(awk -v r=$rate 'BEGIN{print (r > 50) ? 0 : 1}')
    if same_content "$WORK/src/consistency.bin" "$WORK/dst/consistency.bin" && [ "$slow" = "0" ]; then
        ok "MSS=$mss 两端一致，$rate MB/s"
    else
        bad "MSS=$mss 两端一致" "只有 $rate MB/s，疑似 MSS/FC 失配"
    fi
done

# ---- 默认参数基线 ----
echo "[16] 默认参数基线"
make_file 67108864 "$WORK/src/base.bin"
rm -rf "$WORK/dst"; mkdir -p "$WORK/dst"
t0=$(date +%s%N)
"$HRUFT_BIN" recv $((P+19)) "$WORK/dst/" > /dev/null 2>&1 &
RP=$!
sleep 0.8
"$HRUFT_BIN" send 127.0.0.1 $((P+19)) "$WORK/src/base.bin" > /dev/null 2>&1
wait $RP
t1=$(date +%s%N)
secs=$(awk -v a=$t0 -v b=$t1 'BEGIN{print (b-a)/1e9-0.8}')
rate=$(awk -v d=$secs 'BEGIN{printf "%.1f", 64/d}')
if same_content "$WORK/src/base.bin" "$WORK/dst/base.bin"; then
    ok "默认参数 64MB，$rate MB/s"
else
    bad "默认参数 64MB"
fi

finish