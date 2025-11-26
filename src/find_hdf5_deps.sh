#!/bin/bash
set -e # Exit on error

# ==========================================
# CONFIGURATION
# ==========================================
ROOT_DIR="$(pwd)"
SRC_DIR="${ROOT_DIR}/src"
HL_SRC_DIR="${ROOT_DIR}/hl/src"
BUILD_DIR="${ROOT_DIR}/build_temp"

# Include paths (Adjust if your H5pubconf.h is elsewhere)
INCLUDE_DIRS="-I./src -I./hl/src -I./include -I../zlib-1.3.1"

# Output files
REQUIRED_FILE="${ROOT_DIR}/required_sources.txt"
EXCLUDED_FILE="${ROOT_DIR}/excluded_sources.txt"

# Compiler Flags
# -ffunction-sections -fdata-sections: Crucial for fine-grained pruning
CFLAGS="-O2 -ffunction-sections -fdata-sections ${INCLUDE_DIRS}"

# ==========================================
# 1. SETUP
# ==========================================
echo "--- Setting up build environment ---"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
rm -f "$REQUIRED_FILE" "$EXCLUDED_FILE"

# ==========================================
# 2. COMPILE EVERYTHING (First Pass)
# ==========================================
echo "--- Compiling all sources to object files ---"

# Helper function to compile a directory
compile_dir() {
    local src_path=$1
    local type=$2 # "core" or "hl"
    
    echo "Processing $type sources..."
    for c_file in "$src_path"/*.c; do
        [ -e "$c_file" ] || continue
        
        filename=$(basename "$c_file")
        obj_name="${type}_${filename%.*}.o"
        
        # Store full path in specific attribute for later retrieval
        echo "${obj_name}|${c_file}" >> "$BUILD_DIR/file_map.txt"
        
        gcc $CFLAGS -c "$c_file" -o "$BUILD_DIR/$obj_name"
    done
}

compile_dir "$SRC_DIR" "core"
compile_dir "$HL_SRC_DIR" "hl"

cd "$BUILD_DIR"

# ==========================================
# 3. IDENTIFY PUBLIC API (ROOTS)
# ==========================================
echo "--- Identifying Public API Roots ---"

# Extract global symbols starting with H5
# "T" = Text, "B" = BSS, "D" = Data, "R" = ReadOnly
nm -g -P *.o | awk '$1 ~ /^H5/ && ($2 == "T" || $2 == "B" || $2 == "D" || $2 == "R") {print $1}' | sort | uniq > roots.txt

# Create Linker Script to preserve these roots
echo "EXTERN(" > retain.ld
cat roots.txt >> retain.ld
echo ")" >> retain.ld

# ==========================================
# 4. GARBAGE COLLECTED LINK (Simulated)
# ==========================================
echo "--- Performing Garbage Collected Partial Link ---"

# Link all objects into one big object (lib_slim.o)
# --gc-sections will discard any section not reachable from the roots
ld -r --gc-sections -T retain.ld -o lib_slim.o *.o

# ==========================================
# 5. ANALYZE SURVIVORS
# ==========================================
echo "--- Analyzing which source files survived ---"

# Extract all symbols that survived into the slim library
nm -P lib_slim.o | awk '{print $1}' | sort | uniq > kept_symbols.txt

touch "$REQUIRED_FILE"
touch "$EXCLUDED_FILE"

# Iterate over the file map
while IFS="|" read -r obj_file src_path; do
    
    # 1. Get symbols defined in the specific original object
    nm -P --defined-only "$obj_file" 2>/dev/null | awk '{print $1}' > obj_symbols.tmp
    
    # 2. Check for intersection
    #    If any symbol in this object exists in kept_symbols.txt, the file is needed.
    if grep -F -q -f obj_symbols.tmp kept_symbols.txt; then
        echo "$src_path" >> "$REQUIRED_FILE"
    else
        echo "$src_path" >> "$EXCLUDED_FILE"
    fi
    
done < file_map.txt

# ==========================================
# 6. SUMMARY
# ==========================================
cd "$ROOT_DIR"
# rm -rf "$BUILD_DIR" # Uncomment to clean up automatically

req_count=$(wc -l < "$REQUIRED_FILE")
excl_count=$(wc -l < "$EXCLUDED_FILE")

echo "=========================================="
echo "Analysis Complete."
echo "Required files: $req_count (See required_sources.txt)"
echo "Excluded files: $excl_count (See excluded_sources.txt)"
echo "=========================================="
