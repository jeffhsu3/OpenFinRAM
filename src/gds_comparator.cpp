#include <iostream>
#include <cstring>
#include <vector>
#include <map>
#include <cstdlib>
#include <cmath>
#include "gdstk/gdstk.hpp"

using namespace gdstk;

/**
 * 比较两个 Vec2 向量是否相等
 */
bool compare_vec2(const Vec2& v1, const Vec2& v2, double tolerance = 1e-10) {
    return std::abs(v1.x - v2.x) < tolerance && std::abs(v1.y - v2.y) < tolerance;
}

/**
 * 比较两个点数组是否相等
 */
bool compare_point_arrays(const Array<Vec2>& arr1, const Array<Vec2>& arr2, 
                          double tolerance = 1e-10) {
    if (arr1.count != arr2.count) {
        std::cerr << "    Point array count mismatch: " << arr1.count << " vs " << arr2.count << "\n";
        return false;
    }
    for (uint64_t i = 0; i < arr1.count; i++) {
        if (!compare_vec2(arr1[i], arr2[i], tolerance)) {
            std::cerr << "    Point " << i << " mismatch: (" << arr1[i].x << ", " << arr1[i].y 
                      << ") vs (" << arr2[i].x << ", " << arr2[i].y << ")\n";
            return false;
        }
    }
    return true;
}

/**
 * 比较两个 Polygon 是否相等
 */
bool compare_polygons(const Polygon* p1, const Polygon* p2, double tolerance = 1e-10) {
    // 比较 Tag（包含 layer 和 type）
    if (p1->tag != p2->tag) {
        std::cerr << "  Polygon tag mismatch: " << p1->tag << " vs " << p2->tag << "\n";
        return false;
    }
    if (!compare_point_arrays(p1->point_array, p2->point_array, tolerance)) {
        std::cerr << "  Polygon point array mismatch\n";
        return false;
    }
    return true;
}

/**
 * 比较两个 Label 是否相等
 */
bool compare_labels(const Label* l1, const Label* l2, double tolerance = 1e-10) {
    // 比较 Tag（包含 layer 和 type）
    if (l1->tag != l2->tag) {
        std::cerr << "  Label tag mismatch: " << l1->tag << " vs " << l2->tag << "\n";
        return false;
    }
    if (strcmp(l1->text, l2->text) != 0) {
        std::cerr << "  Label text mismatch: '" << l1->text << "' vs '" << l2->text << "'\n";
        return false;
    }
    if (!compare_vec2(l1->origin, l2->origin, tolerance)) {
        std::cerr << "  Label origin mismatch\n";
        return false;
    }
    if (l1->anchor != l2->anchor) {
        std::cerr << "  Label anchor mismatch: " << (int)l1->anchor << " vs " << (int)l2->anchor << "\n";
        return false;
    }
    if (std::abs(l1->rotation - l2->rotation) > tolerance) {
        std::cerr << "  Label rotation mismatch: " << l1->rotation << " vs " << l2->rotation << "\n";
        return false;
    }
    if (std::abs(l1->magnification - l2->magnification) > tolerance) {
        std::cerr << "  Label magnification mismatch: " << l1->magnification << " vs " 
                  << l2->magnification << "\n";
        return false;
    }
    if (l1->x_reflection != l2->x_reflection) {
        std::cerr << "  Label x_reflection mismatch: " << l1->x_reflection << " vs " 
                  << l2->x_reflection << "\n";
        return false;
    }
    return true;
}

/**
 * 前向声明
 */
bool compare_flexpath_elements(const FlexPathElement* elem1, const FlexPathElement* elem2,
                               double tolerance);

/**
 * 比较两个 FlexPath 的元素是否相等
 */
bool compare_flexpath_elements(const FlexPathElement* elem1, const FlexPathElement* elem2,
                               double tolerance) {
    if (elem1->tag != elem2->tag) {
        std::cerr << "    FlexPathElement tag mismatch: " << elem1->tag << " vs " 
                  << elem2->tag << "\n";
        return false;
    }
    if (elem1->join_type != elem2->join_type) {
        std::cerr << "    FlexPathElement join_type mismatch\n";
        return false;
    }
    if (elem1->end_type != elem2->end_type) {
        std::cerr << "    FlexPathElement end_type mismatch\n";
        return false;
    }
    if (!compare_vec2(elem1->end_extensions, elem2->end_extensions, tolerance)) {
        std::cerr << "    FlexPathElement end_extensions mismatch\n";
        return false;
    }
    if (elem1->bend_type != elem2->bend_type) {
        std::cerr << "    FlexPathElement bend_type mismatch\n";
        return false;
    }
    if (std::abs(elem1->bend_radius - elem2->bend_radius) > tolerance) {
        std::cerr << "    FlexPathElement bend_radius mismatch: " << elem1->bend_radius 
                  << " vs " << elem2->bend_radius << "\n";
        return false;
    }
    if (!compare_point_arrays(elem1->half_width_and_offset, elem2->half_width_and_offset, tolerance)) {
        std::cerr << "    FlexPathElement half_width_and_offset mismatch\n";
        return false;
    }
    return true;
}

/**
 * 比较两个 FlexPath 是否相等
 */
bool compare_flexpaths(const FlexPath* fp1, const FlexPath* fp2, double tolerance = 1e-10) {
    if (fp1->num_elements != fp2->num_elements) {
        std::cerr << "  FlexPath num_elements mismatch: " << fp1->num_elements << " vs " 
                  << fp2->num_elements << "\n";
        return false;
    }
    if (fp1->simple_path != fp2->simple_path) {
        std::cerr << "  FlexPath simple_path mismatch: " << fp1->simple_path << " vs " 
                  << fp2->simple_path << "\n";
        return false;
    }
    if (fp1->scale_width != fp2->scale_width) {
        std::cerr << "  FlexPath scale_width mismatch: " << fp1->scale_width << " vs " 
                  << fp2->scale_width << "\n";
        return false;
    }
    
    // 比较 spine（Curve）
    if (fp1->spine.point_array.count != fp2->spine.point_array.count) {
        std::cerr << "  FlexPath spine point count mismatch: " << fp1->spine.point_array.count 
                  << " vs " << fp2->spine.point_array.count << "\n";
        return false;
    }
    if (!compare_point_arrays(fp1->spine.point_array, fp2->spine.point_array, tolerance)) {
        std::cerr << "  FlexPath spine point array mismatch\n";
        return false;
    }
    
    // 比较所有元素
    for (uint64_t i = 0; i < fp1->num_elements; i++) {
        if (!compare_flexpath_elements(&fp1->elements[i], &fp2->elements[i], tolerance)) {
            std::cerr << "  FlexPath element " << i << " mismatch\n";
            return false;
        }
    }
    return true;
}

/**
 * 比较两个 RobustPath 是否相等（简化版本）
 */
bool compare_robustpaths(const RobustPath* rp1, const RobustPath* rp2, double tolerance = 1e-10) {
    // RobustPath 的比较比较复杂，这里做简化比较
    if (rp1->num_elements != rp2->num_elements) {
        std::cerr << "  RobustPath num_elements mismatch: " << rp1->num_elements << " vs " 
                  << rp2->num_elements << "\n";
        return false;
    }
    if (rp1->subpath_array.count != rp2->subpath_array.count) {
        std::cerr << "  RobustPath subpath_array count mismatch: " << rp1->subpath_array.count 
                  << " vs " << rp2->subpath_array.count << "\n";
        return false;
    }
    
    // 比较端点
    if (!compare_vec2(rp1->end_point, rp2->end_point, tolerance)) {
        std::cerr << "  RobustPath end_point mismatch\n";
        return false;
    }
    
    // 比较 tolerance 和 width/offset scale
    if (std::abs(rp1->tolerance - rp2->tolerance) > tolerance) {
        std::cerr << "  RobustPath tolerance mismatch: " << rp1->tolerance << " vs " 
                  << rp2->tolerance << "\n";
        return false;
    }
    if (std::abs(rp1->width_scale - rp2->width_scale) > tolerance) {
        std::cerr << "  RobustPath width_scale mismatch: " << rp1->width_scale << " vs " 
                  << rp2->width_scale << "\n";
        return false;
    }
    if (std::abs(rp1->offset_scale - rp2->offset_scale) > tolerance) {
        std::cerr << "  RobustPath offset_scale mismatch: " << rp1->offset_scale << " vs " 
                  << rp2->offset_scale << "\n";
        return false;
    }
    
    return true;
}

/**
 * 比较两个 Reference 是否相等
 */
bool compare_references(const Reference* ref1, const Reference* ref2) {
    // 比较类型
    if (ref1->type != ref2->type) {
        std::cerr << "  Reference type mismatch: " << (int)ref1->type << " vs " 
                  << (int)ref2->type << "\n";
        return false;
    }
    
    // 根据类型进行比较
    // 注意：对于 Cell 和 RawCell，比较名称而不是指针，因为来自不同 Library 的对象指针不同
    if (ref1->type == ReferenceType::Cell) {
        if (strcmp(ref1->cell->name, ref2->cell->name) != 0) {
            std::cerr << "  Reference cell name mismatch: '" << ref1->cell->name << "' vs '" 
                      << ref2->cell->name << "'\n";
            return false;
        }
    } else if (ref1->type == ReferenceType::RawCell) {
        // RawCell 也应该比较名称
        if (strcmp(ref1->rawcell->name, ref2->rawcell->name) != 0) {
            std::cerr << "  Reference rawcell name mismatch: '" << ref1->rawcell->name << "' vs '" 
                      << ref2->rawcell->name << "'\n";
            return false;
        }
    } else if (ref1->type == ReferenceType::Name) {
        if (strcmp(ref1->name, ref2->name) != 0) {
            std::cerr << "  Reference name mismatch: '" << ref1->name << "' vs '" 
                      << ref2->name << "'\n";
            return false;
        }
    }
    
    // 比较变换参数
    if (!compare_vec2(ref1->origin, ref2->origin, 1e-10)) {
        std::cerr << "  Reference origin mismatch\n";
        return false;
    }
    if (std::abs(ref1->rotation - ref2->rotation) > 1e-10) {
        std::cerr << "  Reference rotation mismatch: " << ref1->rotation << " vs " 
                  << ref2->rotation << "\n";
        return false;
    }
    if (std::abs(ref1->magnification - ref2->magnification) > 1e-10) {
        std::cerr << "  Reference magnification mismatch: " << ref1->magnification << " vs " 
                  << ref2->magnification << "\n";
        return false;
    }
    if (ref1->x_reflection != ref2->x_reflection) {
        std::cerr << "  Reference x_reflection mismatch: " << ref1->x_reflection << " vs " 
                  << ref2->x_reflection << "\n";
        return false;
    }
    return true;
}

/**
 * 比较两个 Cell 的内容（忽略名称之外的元数据）
 */
bool compare_cells(const Cell* cell1, const Cell* cell2, double tolerance = 1e-10) {
    // 比较 polygons
    if (cell1->polygon_array.count != cell2->polygon_array.count) {
        std::cerr << "Polygon count mismatch in cell: " << cell1->name 
                  << " (" << cell1->polygon_array.count << " vs " 
                  << cell2->polygon_array.count << ")\n";
        return false;
    }

    for (uint64_t i = 0; i < cell1->polygon_array.count; i++) {
        if (!compare_polygons(cell1->polygon_array[i], cell2->polygon_array[i], tolerance)) {
            std::cerr << "Polygon " << i << " mismatch in cell " << cell1->name << "\n";
            return false;
        }
    }

    // 比较 labels
    if (cell1->label_array.count != cell2->label_array.count) {
        std::cerr << "Label count mismatch in cell: " << cell1->name << "\n";
        return false;
    }

    for (uint64_t i = 0; i < cell1->label_array.count; i++) {
        if (!compare_labels(cell1->label_array[i], cell2->label_array[i], tolerance)) {
            std::cerr << "Label " << i << " mismatch in cell " << cell1->name << "\n";
            return false;
        }
    }

    // 比较 references
    if (cell1->reference_array.count != cell2->reference_array.count) {
        std::cerr << "Reference count mismatch in cell: " << cell1->name << "\n";
        return false;
    }

    for (uint64_t i = 0; i < cell1->reference_array.count; i++) {
        if (!compare_references(cell1->reference_array[i], cell2->reference_array[i])) {
            std::cerr << "Reference " << i << " mismatch in cell " << cell1->name << "\n";
            return false;
        }
    }

    // 比较 flexpaths
    if (cell1->flexpath_array.count != cell2->flexpath_array.count) {
        std::cerr << "FlexPath count mismatch in cell: " << cell1->name << "\n";
        return false;
    }

    for (uint64_t i = 0; i < cell1->flexpath_array.count; i++) {
        if (!compare_flexpaths(cell1->flexpath_array[i], cell2->flexpath_array[i], tolerance)) {
            std::cerr << "FlexPath " << i << " mismatch in cell " << cell1->name << "\n";
            return false;
        }
    }

    // 比较 robustpaths
    if (cell1->robustpath_array.count != cell2->robustpath_array.count) {
        std::cerr << "RobustPath count mismatch in cell: " << cell1->name << "\n";
        return false;
    }

    for (uint64_t i = 0; i < cell1->robustpath_array.count; i++) {
        if (!compare_robustpaths(cell1->robustpath_array[i], cell2->robustpath_array[i], tolerance)) {
            std::cerr << "RobustPath " << i << " mismatch in cell " << cell1->name << "\n";
            return false;
        }
    }

    return true;
}

/**
 * 比较两个 Library 是否相等（忽略名称和时间戳等元数据）
 */
bool compare_libraries(const Library& lib1, const Library& lib2, double tolerance = 1e-10) {
    // 比较 unit 和 precision
    if (std::abs(lib1.unit - lib2.unit) > tolerance || 
        std::abs(lib1.precision - lib2.precision) > tolerance) {
        std::cerr << "Unit or precision mismatch:\n";
        std::cerr << "  Library 1: unit=" << lib1.unit << ", precision=" << lib1.precision << "\n";
        std::cerr << "  Library 2: unit=" << lib2.unit << ", precision=" << lib2.precision << "\n";
        return false;
    }

    // 比较 cell 数量
    if (lib1.cell_array.count != lib2.cell_array.count) {
        std::cerr << "Cell count mismatch: " << lib1.cell_array.count 
                  << " vs " << lib2.cell_array.count << "\n";
        return false;
    }

    // 构建 cell 名称映射（因为顺序可能不同）
    std::map<std::string, Cell*> cells2_map;
    for (uint64_t i = 0; i < lib2.cell_array.count; i++) {
        cells2_map[lib2.cell_array[i]->name] = lib2.cell_array[i];
    }

    // 逐个比较每个 cell
    for (uint64_t i = 0; i < lib1.cell_array.count; i++) {
        Cell* cell1 = lib1.cell_array[i];
        auto it = cells2_map.find(cell1->name);
        
        if (it == cells2_map.end()) {
            std::cerr << "Cell '" << cell1->name << "' not found in library 2\n";
            return false;
        }

        Cell* cell2 = it->second;
        if (!compare_cells(cell1, cell2, tolerance)) {
            return false;
        }
    }

    std::cout << "All cells match successfully!\n";
    return true;
}

/**
 * 主函数：比较两个 GDS 文件
 */
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <gds_file1> <gds_file2> [tolerance]\n";
        std::cerr << "  gds_file1: Path to first GDS file\n";
        std::cerr << "  gds_file2: Path to second GDS file\n";
        std::cerr << "  tolerance: Floating point comparison tolerance (default: 1e-10)\n";
        return 1;
    }

    const char* file1 = argv[1];
    const char* file2 = argv[2];
    double tolerance = (argc > 3) ? std::atof(argv[3]) : 1e-10;

    std::cout << "Comparing GDS files (ignoring metadata):\n";
    std::cout << "  File 1: " << file1 << "\n";
    std::cout << "  File 2: " << file2 << "\n";
    std::cout << "  Tolerance: " << tolerance << "\n\n";

    // 读取第一个 GDS 文件
    ErrorCode error1 = ErrorCode::NoError;
    Library lib1 = read_gds(file1, 0, 0, nullptr, &error1);
    if (error1 != ErrorCode::NoError) {
        std::cerr << "Error reading file 1: " << (int)error1 << "\n";
        return 1;
    }

    // 读取第二个 GDS 文件
    ErrorCode error2 = ErrorCode::NoError;
    Library lib2 = read_gds(file2, 0, 0, nullptr, &error2);
    if (error2 != ErrorCode::NoError) {
        std::cerr << "Error reading file 2: " << (int)error2 << "\n";
        lib1.free_all();
        return 1;
    }

    // 比较两个库
    bool result = compare_libraries(lib1, lib2, tolerance);

    // 清理内存
    lib1.free_all();
    lib2.free_all();

    if (result) {
        std::cout << "\n✓ GDS files are identical (ignoring metadata)!\n";
        return 0;
    } else {
        std::cout << "\n✗ GDS files are different!\n";
        return 1;
    }
}
