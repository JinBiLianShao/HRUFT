#include "protocol.h"
#include <iostream>
#include <cassert>

using namespace hruft;

void test_control_header() {
    std::cout << "Testing ControlHeader..." << std::endl;
    
    ControlHeader header(ControlType::SYN, 123, 456);
    
    assert(header.magic == HRUF_MAGIC);
    assert(header.version == PROTOCOL_VERSION);
    assert(header.type == ControlType::SYN);
    assert(header.chunkId == 123);
    assert(header.payloadLen == 456);
    
    assert(header.validate() == true);
    
    // 测试无效头
    ControlHeader invalid;
    invalid.magic = 0;
    assert(invalid.validate() == false);
    
    std::cout << "ControlHeader tests passed!" << std::endl;
}

void test_data_header() {
    std::cout << "Testing DataHeader..." << std::endl;
    
    DataHeader header(1, 100, 4096, 1400, LAST_PACKET);
    
    assert(header.magic == HRUF_MAGIC);
    assert(header.version == PROTOCOL_VERSION);
    assert(header.chunkId == 1);
    assert(header.seq == 100);
    assert(header.offset == 4096);
    assert(header.dataLen == 1400);
    assert(header.flags == LAST_PACKET);
    assert(header.crc32 == 0);
    
    assert(header.validate() == true);
    assert(header.isLastPacket() == true);
    assert(header.isRetransmit() == false);
    
    std::cout << "DataHeader tests passed!" << std::endl;
}

void test_data_packet() {
    std::cout << "Testing DataPacket..." << std::endl;
    
    std::vector<uint8_t> testData = {0x01, 0x02, 0x03, 0x04, 0x05};
    
    DataPacket packet(2, 50, 8192, testData.data(), 
                      static_cast<uint16_t>(testData.size()));
    
    // 序列化
    auto serialized = packet.serialize();
    assert(serialized.size() == sizeof(DataHeader) + testData.size());
    
    // 反序列化
    auto deserialized = DataPacket::deserialize(serialized.data(), serialized.size());
    
    assert(deserialized.header.chunkId == 2);
    assert(deserialized.header.seq == 50);
    assert(deserialized.header.offset == 8192);
    assert(deserialized.header.dataLen == testData.size());
    assert(deserialized.data == testData);
    
    // 测试CRC验证
    deserialized.header.crc32 = 0x12345678;
    serialized = deserialized.serialize();
    
    try {
        DataPacket::deserialize(serialized.data(), serialized.size());
        assert(false && "Should have thrown CRC error");
    } catch (const std::runtime_error& e) {
        // 期望的异常
    }
    
    std::cout << "DataPacket tests passed!" << std::endl;
}

void test_protocol_constants() {
    std::cout << "Testing protocol constants..." << std::endl;
    
    assert(HRUF_MAGIC == 0x48525546);
    assert(PROTOCOL_VERSION == 0x0001);
    
    // 测试枚举值
    assert(static_cast<uint8_t>(ControlType::SYN) == 0x01);
    assert(static_cast<uint8_t>(ControlType::FILE_DONE) == 0x06);
    assert(static_cast<uint8_t>(ControlType::ERROR) == 0xFF);
    
    // 测试标志位
    assert(LAST_PACKET == 0x01);
    assert(RETRANSMIT == 0x02);
    assert(ENCRYPTED == 0x04);
    
    std::cout << "Protocol constants tests passed!" << std::endl;
}

void test_crc32() {
    std::cout << "Testing CRC32..." << std::endl;
    
    const uint8_t data1[] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t data2[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    
    uint32_t crc1 = calculate_crc32(data1, sizeof(data1));
    uint32_t crc2 = calculate_crc32(data2, sizeof(data2));
    
    assert(crc1 != crc2);
    
    // 测试一致性
    uint32_t crc1_again = calculate_crc32(data1, sizeof(data1));
    assert(crc1 == crc1_again);
    
    std::cout << "CRC32 tests passed!" << std::endl;
}

int main() {
    std::cout << "Running HRUFT protocol tests..." << std::endl;
    std::cout << "================================" << std::endl;
    
    try {
        test_protocol_constants();
        test_control_header();
        test_data_header();
        test_crc32();
        test_data_packet();
        
        std::cout << std::endl;
        std::cout << "All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }
}