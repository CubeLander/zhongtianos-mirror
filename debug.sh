TARGET_REMOTE="localhost:1234"
PROGRAM="kernel-qemu"

echo "Starting GDB and connecting to $TARGET_REMOTE..."
riscv64-unknown-elf-gdb -ex "target extended-remote $TARGET_REMOTE" \
                        -ex "b *0x8029aac" \
                        -ex "c " \
                        $PROGRAM
                        
                        # -ex "add-symbol-file $USER_PROGRAM 0x0" \
                        #-ex "b main" \ # -ex "symbol-file $USER_PROGRAM" \
                        # -ex "b _mentry" \