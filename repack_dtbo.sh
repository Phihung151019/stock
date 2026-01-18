#!/bin/bash

DTS_DIR="output_dts"
TEMP_DTBO_DIR="repack_dtbo_bin"
OUTPUT_IMG="new_dtbo.img"

# 1. Tạo thư mục tạm để chứa các file binary đã biên dịch
mkdir -p $TEMP_DTBO_DIR

echo "--- Bước 1: Biên dịch .dts sang .dtbo ---"

# Duyệt qua các file .dts trong thư mục
for dts_file in "$DTS_DIR"/*.dts; do
    if [ -f "$dts_file" ]; then
        filename=$(basename "$dts_file" .dts)
        echo "Đang biên dịch: $filename"
        
        # Biên dịch dts -> dtbo
        # -@: Rất quan trọng để giữ lại các symbols cho overlay
        dtc -@ -I dts -O dtb "$dts_file" -o "$TEMP_DTBO_DIR/$filename"
    fi
done

echo "--- Bước 2: Đóng gói thành $OUTPUT_IMG ---"

# Tạo danh sách các file để đóng gói (theo đúng thứ tự số)
# Lệnh 'sort -V' giúp sắp xếp dtbo.0, dtbo.1... đúng thứ tự tự nhiên
FILES=$(ls "$TEMP_DTBO_DIR" | sort -V)
DTBO_LIST=""
for f in $FILES; do
    DTBO_LIST+="$TEMP_DTBO_DIR/$f "
done

# Sử dụng mkdtboimg.py để tạo image cuối cùng
python3 mkdtboimg.py create "$OUTPUT_IMG" $DTBO_LIST

echo "--------------------------------------"
echo "Xong! File mới đã được tạo: $OUTPUT_IMG"
