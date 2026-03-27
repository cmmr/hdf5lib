
# These functions perform several internal 
# tests and call `stop()` on invalid states. 
expect_silent(x <- c_flags())
expect_silent(y <- ld_flags())

# Expect a string. Not NA or "".

expect_inherits(x, "character")
expect_inherits(y, "character")
expect_length(x, 1)
expect_length(y, 1)
expect_false(is.na(x))
expect_false(is.na(y))
expect_true(nzchar(x))
expect_true(nzchar(y))
  
