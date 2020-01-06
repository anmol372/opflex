/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*
 * Include file for PacketDecoder
 *
 * Copyright (c) 2019 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */
#ifndef OPFLEXAGENT_PACKETDECODER_H
#define OPFLEXAGENT_PACKETDECODER_H

#include <memory>
#include <unordered_map>
#include <set>
#include <vector>
#include <iostream>
#include <string>
#include <sstream>
#include <boost/format.hpp>

namespace opflexagent {

class PacketDecoder;

/**
 *  Struct to hold parsing context
 */
struct ParseInfo {
    /**
     * Constructor for ParseInfo
     * @param _decoder pointer to the PacketDecoder instance that is
     * driving the parsing
     */
    ParseInfo(PacketDecoder *_decoder):pktDecoder(_decoder),nextLayerTypeId(0),
            nextKey(0), optionLayerTypeId(0), parsedLength(0), parsedString(),
            formattedFields(), layerFormatterString(), hasOptBytes(false),
            pendingOptionLength(0), inferredLength(0), inferredDataLength(0),
            scratchpad{0,0,0,0} {};
    /**
     * Packet decoder instance
     */
    PacketDecoder *pktDecoder;
    /**
     * Next Layer Type Id
     */
    uint32_t nextLayerTypeId;
    /**
     * Next Layer Key
     */
    uint32_t nextKey;
    /**
     * Option Layer Type Id
     */
    uint32_t optionLayerTypeId;
    /**
     * Bytes parsed by current layer
     */
    uint32_t parsedLength;
    /**
     * Parsed output
     */
    std::string parsedString;
    /**
     * Field values in the format string of a given layer
     */
    std::vector<std::string> formattedFields;
    /**
     * Format string of a given layer
     */
    boost::format layerFormatterString;
    /**
     * Layer has variable length data
     */
    bool hasOptBytes;
    /**
     * pending Option length of a layer
     */
    uint32_t pendingOptionLength;
    /**
     * Inferred header length for variable length header
     */
    uint32_t inferredLength;
    /**
     * Inferred data length for variable length header
     */
    uint32_t inferredDataLength;
    /**
     * Scratchpad to store 4 select field values in a layer
     */
    uint32_t scratchpad[4];
};

/* Allowed header field types */
typedef enum  {
    FLDTYPE_NONE,
    FLDTYPE_BITFIELD,
    FLDTYPE_BYTES,
    FLDTYPE_IPv4ADDR,
    FLDTYPE_IPv6ADDR,
    FLDTYPE_MAC,
    FLDTYPE_VARBYTES,
    FLDTYPE_OPTBYTES
} PacketDecoderLayerFieldType;

/**
 * Class to represent a packet header field
 */
class PacketDecoderLayerField {
public:
    /**
     * Constructor for a packet header field
     * @param name Name
     * @param len length
     * @param offset offset of this field from the start of the header
     * @param type field data type
     * @param nextKey true if field is a key to the next layer
     * @param length true if this field holds the length of this header
     * @param _scratchOffset scratchpad offset that this value should go to.
     * @param printSeq formatted position in the layer output
     */
    PacketDecoderLayerField(std::string &name, uint32_t len, uint32_t offset,
            PacketDecoderLayerFieldType type, bool nextKey=false,
            bool length=false, int _scratchOffset = -1, int printSeq = 0):
        fieldType(type), fieldName(name), bitLength(len), bitOffset(offset),
        isNextKey(nextKey), isLength(length), scratchOffset(_scratchOffset),
        outSeq(printSeq) {}
    /**
     * Whether matching traffic should be allowed or dropped
     * @return true if this field indicates the length of the containing Layer
     */
    bool getIsLength() {return isLength;}
    /**
     * decode the bytes in the given buffer as this header field.
     * @param buf input buffer
     * @param length total length of buffer
     * @param p ParseInfo structure which contains more context for parsing
     * @return true if required number of bits were extracted and valid.
     */
    int decode(const unsigned char *buf, std::size_t length, ParseInfo &p);
    /**
     * populate human readable strings for specific field values as a map
     * @param outMap value to string map for field values.
     */
    void populateOutMap(std::unordered_map<uint32_t,std::string> &outMap);
private:
    PacketDecoderLayerFieldType fieldType;
    std::string fieldName;
    uint32_t bitLength;
    uint32_t bitOffset;
    bool isNextKey,isLength;
    int scratchOffset, outSeq;
    std::unordered_map<uint32_t, std::string> kvOutMap;
    bool getIsNextKey() {return isNextKey;}
    bool shouldSave(int &_offset) {_offset=scratchOffset; return (_offset != -1);}
    bool shouldLog() {return (outSeq != 0);}
    void transformLog(uint32_t val, std::stringstream &ostr, ParseInfo &p);
};

/**
 * Class to represent a packet header
 */
class PacketDecoderLayer {
public:
    /**
     * Constructor for PacketDecoderLayer
     * @param typeName name of the base layer type
     * @param _key key value for this layer
     * @param name name of this layer
     * @param len length of this layer
     * @param nextLayer name of the next layer type
     * @param optionLayer name of the associated option layer
     * @param lTypeId id of the base layer type
     * @param lId running number id of this layer
     * @param nLId next layer type id
     * @param optTypeId option layer type id
     * @param optId running number id of option layer
     * @param num_args Number of arguments in the format string
     */
    PacketDecoderLayer(std::string typeName, uint32_t _key, std::string name,
            uint32_t len, std::string nextLayer, std::string optionLayer,
            uint32_t lTypeId, uint32_t lId, uint32_t nLId, uint32_t optTypeId,
            uint32_t optId, uint32_t num_args):
        layerTypeName(typeName), layerName(name), nextTypeName(nextLayer),
        optionLayerName(optionLayer), byteLength(len), key(_key),
        layerTypeId(lTypeId), layerId(lId), nextTypeId(nLId),
        optionLayerTypeId(optTypeId),optionLayerId(optId),
        numOutArgs(num_args) {
        amOptionLayer = (layerTypeName == layerName) && (key == 0);
    };
    /**
     * Whether this layer supports option headers
     * @return If option headers are supported
     */
    bool haveOptions() {return (optionLayerId != 0);}
    /**
     * Whether this layer is an option layer.
     * @return if this is an option layer
     */
    bool isOptionLayer() {return amOptionLayer;}
    /**
     * Get the Id of this layer
     * @return layer id
     */
    uint32_t getId() { return layerId; }
    /**
     * Get the name of the base layer type
     * @return reference to base layer type
     */
    const std::string &getTypeName() { return layerTypeName;}
    /**
     * Get the name of the this layer
     * @return reference to layer name
     */
    const std::string &getName() { return layerName;}
    /**
     * Get the name of the next layer type
     * @return reference to next layer type name
     */
    const std::string &getNextTypeName() { return nextTypeName;}
    /**
     * Get the name of the option layer associated with this layer
     * @return reference to option layer name
     */
    const std::string &getOptionLayerName() { return optionLayerName;}
    /**
     * Get the id of the base layer type
     * @return base layer type id
     */
    uint32_t getTypeId() { return layerTypeId; }
    /**
     * Get the id of the base layer type
     * @return base layer type id
     */
    uint32_t getKey() { return key; }
    /**
     * Get the id of the next layer type
     * @return next layer type id
     */
    uint32_t getNextTypeId() { return nextTypeId; }
    /**
     * Get the id of the option base layer type
     * @return option base layer type id
     */
    uint32_t getOptionLayerTypeId() {return optionLayerTypeId; }
    /**
     * Get the id of the option layer
     * @return option layer id
     */
    uint32_t getOptionLayerId() {return optionLayerId; }
    /**
     * Extract the field value from the given buffer
     * @param buf buffer to extract bytes from
     * @param length total length of the buffer
     * @param p parsing context
     * @return true if parsing succeeded
     */
    virtual int decode(const unsigned char *buf, std::size_t length,
            ParseInfo &p);
    /**
     * Configure the layer and contained fields
     * @return 0 if successfully configured
     */
    virtual int configure()=0;
    /**
     * If this layer is an option layer, get it's computed length
     * @param p Parsing Context and output
     */
    virtual void getOptionLength(ParseInfo &p) {;}
    /**
     * If this layer has a variable data field, get it's computed length
     * @param hdrLength length of the header
     * @return variable data length
     */
    virtual uint32_t getVariableDataLength(uint32_t hdrLength) {return 0;}
    /**
     * If this layer has variable length, get it's computed length
     * @param fldVal value of the length field
     * @return variable header length
     */
    virtual uint32_t getVariableHeaderLength(uint32_t fldVal) {return fldVal;}
    /**
     * If this layer is an option layer, get its computed length
     * @param p Parsing Context and output
     */
    virtual bool hasOptBytes(ParseInfo &p) {return 0;}
    /**
     * Get the format string for this layer's output
     * @param fmtStr format String for layer output
     */
    virtual void getFormatString(boost::format &fmtStr)=0;
protected:
    ///@{
    /** Layer Identifiers as mentioned */
    std::string layerTypeName,layerName, nextTypeName, optionLayerName;
    ///@}
    ///@{
    /** length in bytes of layer and key for its base layer */
    uint32_t byteLength, key;
    ///@}
    ///@{
    /** Layer Identifiers as mentioned */
    uint32_t layerTypeId, layerId, nextTypeId, optionLayerTypeId,
    optionLayerId, numOutArgs;
    ///@}
    /**
     * Fields in the layer
     */
    std::vector<PacketDecoderLayerField> pktFields;
    /**
     * This is an option header layer
     */
    bool amOptionLayer;
    /**
     * Add a field to this layer
     * @param name field name
     * @param len field bit length
     * @param offset field offset from the start of layer
     * @param fldType type of the field
     * @param nextKey true if this field is the next layer key
     * @param isLength true if this field holds the variable length of header
     * @param scratchOffset index of scratchpad buffer to which this
     * field will be moved
     * @param printSeq output sequence number in layer format string
     * @return 0 if no errors
     */
    int addField(std::string name, uint32_t len, uint32_t offset,
            PacketDecoderLayerFieldType fldType, bool nextKey=false,
            bool isLength=false, int scratchOffset=-1, int printSeq = -1) {
        pktFields.push_back(PacketDecoderLayerField(name, len, offset, fldType,
                nextKey, isLength, scratchOffset, printSeq));
        return 0;
    };
};

/**
 *  Packet Decoder main interface
 */
class PacketDecoder {
public:
    /**
     * Constructor for PacketDecoder
     */
    PacketDecoder():baseLayerId(0){};
    /**
     * Customize PacketDecoder by adding layers inside this method
     * @return true if no errors occurred during configuration
     */
    int configure();
    /**
     * Get corresponding layer for the given id
     * @param id layer id
     * @return shared pointer to Packet decoder layer for this id
     */
    std::shared_ptr<PacketDecoderLayer> getLayerById(uint32_t id) {
        auto it = layerIdMap.find(id);
        if(it != layerIdMap.end()) {
            return it->second;
        }
        return nullptr;
    }
    /**
     * Get corresponding identifier for the given layer name
     * @param name layer name
     * @return layer id
     */
    uint32_t getLayerIdByName(std::string &name) {
        auto it = layerNameMap.find(name);
        if(it != layerNameMap.end()) {
            return it->second;
        }
        return 0;
    }
    /**
     * Get layer using base layer type id and key
     * @param typeId base layer type id
     * @param key key for the specific layer
     * @param p matched PacketDecoderLayer
     * @return true if lookup was successful
     */
    bool getLayerByTypeKey(uint32_t typeId, uint32_t key,
            std::shared_ptr<PacketDecoderLayer> &p ) {
       auto it = decoderMapRegistry.find(typeId);
       if(it == decoderMapRegistry.end()) {
           return false;
       }
       auto it2 = it->second.find(key);
       if(it2 == it->second.end()) {
           return false;
       }
       p = it2->second;
       return true;
    }
    /**
     * Get layer name by type and key
     * @param typeId base layer type id
     * @param key key for the specific layer
     * @param layerName matched layer name
     *
     */
    bool getLayerNameByTypeKey(uint32_t typeId, uint32_t key,
            std::string &layerName) {
        std::shared_ptr<PacketDecoderLayer> p;
        if(getLayerByTypeKey(typeId, key, p)) {
            layerName = p->getName();
            return true;
        }
        return false;
    }
    /**
     * Decode the given buffer using configured layers
     * @param buf packet buffer to decode
     * @param length length of the buffer
     * @param p context for parsing
     * @return true if decoding was error free
     */
    int decode(const unsigned char *buf, std::size_t length, ParseInfo &p);
private:
    std::unordered_map<std::string, uint32_t> layerTypeMap;
    std::unordered_map<std::string, uint32_t> layerNameMap;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t,
            std::shared_ptr<PacketDecoderLayer>>> decoderMapRegistry;
    std::unordered_map<uint32_t, std::shared_ptr<PacketDecoderLayer>> layerIdMap;
    uint32_t baseLayerId;
    void registerLayer(std::shared_ptr<PacketDecoderLayer>&);
};

}
#endif