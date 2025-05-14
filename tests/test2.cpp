#include <iostream>
#include <vector>

std::vector<std::vector<int>> multiplyMatrices(const std::vector<std::vector<int>>& matrix1, const std::vector<std::vector<int>>& matrix2) {
    int rows1 = matrix1.size();
    int cols1 = matrix1[0].size();
    int rows2 = matrix2.size();
    int cols2 = matrix2[0].size();

    if (cols1 != rows2) {
        std::cerr << "Error: Incompatible matrix dimensions for multiplication.\n";
        return {}; 
    }

    std::vector<std::vector<int>> result(rows1, std::vector<int>(cols2, 0));

    for (int i = 0; i < rows1; ++i) {
        for (int j = 0; j < cols2; ++j) {
            for (int k = 0; k < cols1; ++k) {
                result[i][j] += matrix1[i][k] * matrix2[k][j];
            }
        }
    }

    return result;
}

int main() {
    size_t n = 500;
    std::vector<std::vector<int>> matrix1(n, std::vector<int>(n));
    std::vector<std::vector<int>> matrix2(n, std::vector<int>(n));

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            matrix1[i][j] = i + j;
            matrix2[i][j] = i - j;
        }
    }

    std::vector<std::vector<int>> result = multiplyMatrices(matrix1, matrix2);

    if (!result.empty()) {
        std::cout << "Resultant Matrix:\n";
        for (const auto& row : result) {
            for (int val : row) {
                std::cout << val << " ";
            }
            std::cout << "\n";
        }
    }
    return 0;
}