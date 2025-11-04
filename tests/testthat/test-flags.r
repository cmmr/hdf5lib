test_that("c_flags and ld_flags", {
  
  # These functions perform several internal 
  # tests and call `stop()` on invalid states. 
  x <- expect_silent(c_flags())
  y <- expect_silent(ld_flags())
  
  # Expect a string. Not NA or "".
  expect_vector(x, character(0), 1)
  expect_vector(y, character(0), 1)
  expect_false(is.na(x))
  expect_false(is.na(y))
  expect_true(nzchar(x))
  expect_true(nzchar(y))
  
})
