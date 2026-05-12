#!/bin/bash
#SBATCH --job-name=cs4352_a2
#SBATCH --partition=nocona
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --mem-per-cpu=1024M
#SBATCH --time=00:05:00

# Permissions
chmod +x ./decrypt
chmod +x ./transform

# Load the GNU compiler module
module load gcc/14.2.0

# Create a2_results.txt and write full name
echo "Larry To" > a2_results.txt

# Append R# 
echo "11615587" >> a2_results.txt

# Decrypt, capture the file path it outputs
DECRYPT_PATH=$(./decrypt /lustre/work/errees/courses/cs4352/assignment2/input/encrypted.txt)

# Transform, capture the line number (pass only the digits)
LINE_NUM=$(./transform 11615587)

# Extract line N from the decrypted file path, append to results
sed -n "${LINE_NUM}p" "$DECRYPT_PATH" >> a2_results.txt
