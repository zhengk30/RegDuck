#include "../include/table/table.hpp"

//
// ColumnDataPointer class methods
//
//
ColumnDataPointer::ColumnDataPointer(uint64_t row_start, uint64_t tuple_count, StorageBlock& other) {
    row_start_ = row_start;
    tuple_count_ = tuple_count;
    pointer_ = other;
}

uint64_t ColumnDataPointer::GetRowStart() {
    return row_start_;
}

uint64_t ColumnDataPointer::GetTupleCount() {
    return tuple_count_;
}

uint64_t ColumnDataPointer::GetBlockId() {
    return pointer_.GetBlockId();
}

uint64_t ColumnDataPointer::GetBlockOffset() {
    return pointer_.GetBlockOffset();
}

string Table::GetString(idx_t i) {
    assert(i < data.size());
    return data[i];
}

Table::Table(CatalogType type, string catalog, string schema, uint8_t on_conflict,
            bool temporary, bool internal, string sql, string table, uint8_t ncols,
            uint64_t nrows, uint64_t row_group_count) :
            type_(type), catalog_name_(catalog), schema_name_(schema), on_conflict_(on_conflict),
            temporary_(temporary), internal_(internal), sql_(sql), table_name_(table), ncols_(ncols),
            nrows_(nrows), row_group_count_(row_group_count) {
}

ColumnInfo::ColumnInfo() {}

void Table::LoadData(const char* path) {
    auto start = chrono::high_resolution_clock::now();
    uint64_t count = 0;
    ifstream file(path, ios::binary);
    auto file_size = filesystem::file_size(filesystem::path(path));
    byte_t block[DEFAULT_BLOCK_SIZE];
    for (auto& pointer : data_pointers_) {
        uint64_t block_id = pointer.GetBlockId();
        uint64_t block_offset = pointer.GetBlockOffset();
        uint64_t tuple_count = pointer.GetTupleCount();
        uint64_t block_start = HEADER_SIZE * 3 + block_id * DEFAULT_BLOCK_SIZE;
        file.seekg(block_start, ios::beg);
        auto read_size = GET_READ_SIZE(file, file_size);
        file.read(reinterpret_cast<char *>(block), read_size);

        byte_t* cursor = block + CHECKSUM_SIZE + block_offset + sizeof(uint32_t);
        DataReader offset_reader(cursor);
        uint32_t dict_end_offset = offset_reader.Read<uint32_t>();
        uint32_t offset_array[tuple_count];
        offset_reader.Read<uint32_t>(offset_array, tuple_count);
        // printf("offset_vector.size()=%llu\n", offset_vector.size());
        uint32_t total_length = offset_array[tuple_count-1];

        byte_t* string_start = block + CHECKSUM_SIZE + block_offset + dict_end_offset - total_length;
        // string new_chars(reinterpret_cast<char *>(bytes));
        // characters += new_chars;

        DataReader string_reader(string_start);
        for (uint64_t i = 0; i < tuple_count; i++) {
            auto j = tuple_count - 1 - i;
            auto length = offset_array[j];
            if (j > 0) {
                length -= offset_array[j-1];
            }
            // std::cout << length << '\n';
            char row[length];
            string_reader.Read<char>(row, length);
            data.push_back(string(row, length));
        }
    
    }
    auto end = chrono::high_resolution_clock::now();
    chrono::duration<double> elapsed = end - start;
    // cout << "count: " << data.size() <<  ", elapsed: " << elapsed.count() << '\n';
    file.close();
}

void Table::LoadTableColumns(field_id_t field_id, Reader& reader) {
    assert(reader.Read<field_id_t>() == field_id);
    ncols_ = reader.Read<uint8_t>(100);
    for (auto i = 0; i < ncols_; i++) {
        ColumnInfo column_info;
        ColumnInfo::Deserialize(column_info, reader);
        columns_meta_.push_back(column_info);
        assert(reader.Read<field_id_t>() == OBJECT_END);
    }
    assert(reader.Read<field_id_t>() == OBJECT_END);
}

idx_t Table::GetTableStartBlockId() {
    return table_start_.GetBlockId();
}

idx_t Table::GetTableStartBlockIndex() {
    return table_start_.GetBlockIndex();
}

idx_t Table::GetTableStartBlockOffset() {
    return table_start_.GetBlockOffset();
}

uint64_t Table::GetRowCount() {
    return nrows_;
}

void Table::SetRowGroupCount(uint64_t count) {
    row_group_count_ = count;
}

void Table::ReadRowCount(field_id_t field_id, Reader& reader) {
    nrows_ = reader.ReadEncoded<uint64_t>(field_id);
}

void Table::ReadIndexPointers(field_id_t field_id, Reader& reader) {
    auto count = reader.ReadEncoded<uint64_t>(field_id);
    assert(count == 0);
}

void ColumnInfo::Deserialize(ColumnInfo& info, Reader& reader) {
    info.name = reader.Read<string>(100);
    info.logical_type = LogicalType::Deserialize(101, reader);
    info.column_type = ColumnType::Deserialize(103, reader);
    info.compression_type = CompressionType::Deserialize(104, reader);
}

void Table::Deserialize(Reader& reader, Table* table) {
    uint8_t count = reader.Read<uint8_t>(100);
    assert(count == 1);

    // read in table info
    table->type_ = (CatalogType)reader.Read<uint8_t>(100);
    table->catalog_name_ = reader.Read<string>(101);
    table->catalog_name_ = reader.Read<string>(102);
    table->temporary_ = reader.Read<bool>(103);
    table->internal_ = reader.Read<bool>(104);
    table->on_conflict_ = reader.Read<uint8_t>(105);
    table->sql_ = reader.Read<string>(106);
    
    table->table_name_ = reader.Read<string>(200);
    // printf("catalog_name=%s\nschema_name=%s\ntable_name=%s\n", table->catalog_name_.c_str(), table->catalog_name_.c_str(), table->table_name_.c_str());
    table->LoadTableColumns(201, reader);
    assert(reader.Read<field_id_t>() == OBJECT_END);
    
    // printf("about to load table_start_...\n");
    table->table_start_ = MetadataBlock::Deserialize(101, reader);

    // printf("about to load row count...\n");
    table->ReadRowCount(102, reader);
    table->ReadIndexPointers(103, reader);
}

void Table::AddRowGroup(RowGroup* row_group) {
    row_groups_.push_back(row_group);
}

void Table::AddColumnDataPointer(uint64_t row_start, uint64_t tuple_count, StorageBlock block) {
    data_pointers_.push_back(ColumnDataPointer(row_start, tuple_count, block));
}

uint64_t Table::GetRowGroupCount() {
    return row_group_count_;
}

RowGroup* Table::GetRowGroup(idx_t i) {
    assert(i < row_group_count_);
    return row_groups_[i];
}

// void Table::ParseData(byte_t* block, uint64_t read_size, uint64_t tuple_count, uint64_t block_offset) {
//     byte_t* cursor = block + CHECKSUM_SIZE + block_offset + sizeof(uint32_t);
//     DataReader offset_reader(cursor);
//     uint32_t dict_end_offset = offset_reader.Read<uint32_t>();
//     uint32_t offset_array[tuple_count];
//     offset_reader.Read<uint32_t>(offset_array, tuple_count);
//     vector<uint32_t> offset_vector(
//         offset_array, offset_array+sizeof(offset_array)/sizeof(offset_array[0])
//     );
//     // printf("offset_vector.size()=%llu\n", offset_vector.size());
//     uint32_t total_length = offset_array[tuple_count-1];

//     byte_t* string_start = block + CHECKSUM_SIZE + block_offset + dict_end_offset - total_length;
//     // string new_chars(reinterpret_cast<char *>(bytes));
//     // characters += new_chars;

//     DataReader string_reader(string_start);
//     auto cons = end + start - 1;
//     for (uint64_t i = 0; i < tuple_count; i++) {
//         auto j = cons - i;
//         auto length = offset_array[j];
//         if (j > 0) {
//             length -= offset_array[j-1];
//         }
//         char row[length];
//         string_reader.Read<char>(row, length);
//         // TODO: store the string while minimizing contention
        
//     }
    
//     // uint64_t nrows_per_thread = tuple_count / NTHREADS;
//     // vector<thread> threads;
//     // for (uint8_t i = 0; i < NTHREADS; i++) {
//     //     uint64_t start = i * nrows_per_thread;
//     //     uint64_t end = (i == NTHREADS - 1) ? tuple_count : (i + 1) * nrows_per_thread;
//     //     threads.emplace_back(
//     //         parse_data_worker, ref(string_reader), start, end, ref(offset_vector)
//     //     );
//     // }
//     // for (auto& t : threads) {
//     //     t.join();
//     // }
    
//     // parse_data_worker(string_reader, 0, tuple_count, offset_vector);

    
// }
