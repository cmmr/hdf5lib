/* src/dummy.c */


/* 
 * This file exists solely to force R to process the src/ directory,
 * which in turn ensures that the install.libs.R script is executed.
 * 
 * We declare a single dummy function to prevent "empty translation unit"
 * warnings from strict C compilers during R CMD check.
 */


void hdf5lib_dummy_function(void) {}
