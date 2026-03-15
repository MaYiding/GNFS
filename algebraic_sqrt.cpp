// In src/sqrt/algebraic_sqrt.cpp:

// 修改前: result.assign(matrix.rows, false);
result.assign(matrix.rows(), false);  // rows 是方法，需要加 ()
