/*
 Copyright 2015 Nervana Systems Inc.
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

#include <assert.h>

#include <vector>
#include <cstdio>
#include <iostream>
#include <chrono>
#include <utility>
#include <algorithm>

#include "archive.hpp"
#include "media.hpp"
#include "matrix.hpp"
#include "device.hpp"

class DecodeThreadPool : public ThreadPool {
public:
    DecodeThreadPool(int count, int batchSize,
                     int datumSize, int targetSize,
                     BufferPool& in, BufferPool& out,
                     Device* device, Media* media)
    : ThreadPool(count),
      _itemsPerThread((batchSize - 1) / count + 1),
      _in(in), _out(out), _endSignaled(0),
      _manager(0), _stopManager(false), _managerStopped(false), _inputBuf(0),
      _bufferIndex(0), _batchSize(batchSize),
      _datumSize(datumSize),
      _targetSize(targetSize),
      _device(device), _media(media) {
        assert(_itemsPerThread * count >= _batchSize);
        assert(_itemsPerThread * (count - 1) < _batchSize);
        for (int i = 0; i < count; i++) {
            _startSignaled.push_back(0);
            _startInds.push_back(0);
            _endInds.push_back(0);
            _dataOffsets.push_back(0);
            _targetOffsets.push_back(0);
            _targetSpans.push_back(0);
        }
    }

    virtual ~DecodeThreadPool() {
        if (_manager != 0) {
            _manager->join();
            delete _manager;
        }
        // The other thread objects are freed in the destructor
        // of the parent class.
    }

    virtual void start() {
        for (int i = 0; i < _count; i++) {
            _threads.push_back(new thread(&DecodeThreadPool::run, this, i));
        }
        _manager = new thread(&DecodeThreadPool::manage, this);
    }

    virtual void stop() {
        ThreadPool::stop();
        while (stopped() == false) {
            std::this_thread::yield();
            _in.advanceWritePos();
            _in.signalNonEmpty();
        }

        _stopManager = true;
        while (_managerStopped == false) {
            std::this_thread::yield();
            _in.advanceWritePos();
            _in.signalNonEmpty();
            _endSignaled++;
            _ended.notify_one();
        }
    }

protected:
    virtual void run(int id) {
        assert(id < _count);
        _startInds[id] = id * _itemsPerThread;
        int itemCount = _itemsPerThread;
        if (id == _count - 1) {
            itemCount = _batchSize - id * _itemsPerThread;
        }

        _endInds[id] = _startInds[id] + itemCount;
        _dataOffsets[id] = _startInds[id] * _datumSize;
        _targetOffsets[id] = _startInds[id] * _targetSize;
        _targetSpans[id] = itemCount * _targetSize;
        while (_done == false) {
            work(id);
        }

        _stopped[id] = true;
    }

    virtual void work(int id) {
        // Thread function.
        {
            unique_lock<mutex> lock(_mutex);
            while (_startSignaled[id] == 0) {
                _started.wait(lock);
                if (_done == true) {
                    return;
                }
            }
            _startSignaled[id]--;
            assert(_startSignaled[id] == 0);
        }

        int start = _startInds[id];
        int end = _endInds[id];
        // No locking required because threads
        // write into non-overlapping regions.
        BufferPair& outBuf = _out.getForWrite();
        char* dataBuf = outBuf.first->_data + _dataOffsets[id];
        // Handle the data.
        for (int i = start; i < end; i++) {
            int itemSize = 0;
            char* item = _inputBuf->first->getItem(i, itemSize);
            if (item == 0) {
                return;
            }
            _media->transform(item, itemSize, dataBuf, _datumSize);
            dataBuf += _datumSize;
        }

        // Handle the targets.
        char* targetDst = outBuf.second->_data + _targetOffsets[id];
        int targetSize = 0;
        char* targetSrc = _inputBuf->second->getItem(start, targetSize);
        assert(targetSize == _targetSize);
        memcpy(targetDst, targetSrc, _targetSpans[id]);

        {
            lock_guard<mutex> lock(_mutex);
            _endSignaled++;
            assert(_endSignaled <= _count);
        }
        _ended.notify_one();
    }

    void produce() {
        // Produce a minibatch.
        {
            unique_lock<mutex> lock(_out.getMutex());
            while (_out.full() == true) {
                _out.waitForNonFull(lock);
            }
            {
                lock_guard<mutex> lock(_mutex);
                for (unsigned int i = 0; i < _startSignaled.size(); i++) {
                    _startSignaled[i] = 1;
                }
            }
            _started.notify_all();
            {
                unique_lock<mutex> lock(_mutex);
                while (_endSignaled < _count) {
                    _ended.wait(lock);
                }
                _endSignaled = 0;
            }
            // At this point, we have decoded data for the whole minibatch.
            BufferPair& outBuf = _out.getForWrite();
            // TODO: The transpose operation needs to be aware of the
            // underlying data type's size.
            Matrix<char>::transpose(outBuf.first->_data,
                                    _batchSize, _datumSize);
            // Copy to device.
            _device->copyData(_bufferIndex, outBuf.first->_data,
                              outBuf.first->_size);
            _device->copyLabels(_bufferIndex, outBuf.second->_data,
                                outBuf.second->_size);
            _bufferIndex = (_bufferIndex == 0) ? 1 : 0;
            _out.advanceWritePos();
        }
        _out.signalNonEmpty();
    }

    void consume() {
        // Consume an input buffer.
        {
            unique_lock<mutex> lock(_in.getMutex());
            while (_in.empty() == true) {
                _in.waitForNonEmpty(lock);
                if (_stopManager == true) {
                    return;
                }
            }
            _inputBuf = &_in.getForRead();
            produce();
            _in.advanceReadPos();
        }
        _in.signalNonFull();
    }

    void manage() {
        // Thread function.
        int result = _device->init();
        if (result != 0) {
            _stopManager = true;
        }
        while (_stopManager == false) {
            consume();
        }
        _managerStopped = true;
    }

private:
    int                         _itemsPerThread;
    BufferPool&                 _in;
    BufferPool&                 _out;
    mutex                       _mutex;
    condition_variable          _started;
    condition_variable          _ended;
    vector<int>                 _startSignaled;
    int                         _endSignaled;
    thread*                     _manager;
    bool                        _stopManager;
    bool                        _managerStopped;
    BufferPair*                 _inputBuf;
    int                         _bufferIndex;
    int                         _batchSize;
    vector<int>                 _startInds;
    vector<int>                 _endInds;
    vector<int>                 _dataOffsets;
    vector<int>                 _targetOffsets;
    vector<int>                 _targetSpans;
    int                         _datumSize;
    int                         _targetSize;
    Device*                     _device;
    Media*                      _media;
};

class ReadThread: public ThreadPool {
public:
    ReadThread(BufferPool& out, Reader* reader)
    : ThreadPool(1), _out(out), _reader(reader) {
        assert(_count == 1);
    }

protected:
    virtual void work(int id) {
        produce();
    }

    void produce() {
        // Fill input buffers.
        {
            unique_lock<mutex> lock(_out.getMutex());
            while (_out.full() == true) {
                _out.waitForNonFull(lock);
            }
            BufferPair& bufPair = _out.getForWrite();
            int result = _reader->read(bufPair);
            if (result == -1) {
                _done = true;
                throw std::runtime_error("Could not read data\n");
            }
            _out.advanceWritePos();
        }
        _out.signalNonEmpty();
    }

private:
    BufferPool&                 _out;
    Reader*                     _reader;
};

class Loader {
public:
    Loader(int* itemCount, int batchSize,
           const char* repoDir, const char* archiveDir,
           const char* indexFile, const char* metaFile,
           const char* archivePrefix,
           bool shuffle, bool reshuffle,
           int startFileIdx,
           int datumSize, int targetSize, int subsetPercent,
           MediaParams* mediaParams,
           DeviceParams* deviceParams,
           MediaParams* ingestParams)
    : _first(true),
      _batchSize(batchSize), _datumSize(datumSize), _targetSize(targetSize),
      _readBufs(0), _decodeBufs(0), _readThread(0), _decodeThreads(0),
      _device(0), _reader(0), _media(0) {
        _device = Device::create(deviceParams);
        _media = Media::create(mediaParams, ingestParams);
        _reader = new ArchiveReader(itemCount, batchSize, repoDir, archiveDir,
                                    indexFile, metaFile, archivePrefix,
                                    shuffle, reshuffle,
                                    startFileIdx,
                                    subsetPercent, _media);
    }

    virtual ~Loader() {
        delete _readBufs;
        delete _readThread;
        delete _decodeBufs;
        delete _decodeThreads;
        delete _device;
        delete _reader;
        delete _media;
    }

    int start() {
        _first = true;
        try {
            // TODO: Read buffers could be smaller because we
            // usually deal with compressed data.
            _readBufs = new BufferPool(_batchSize * _datumSize,
                                       _batchSize * _targetSize);
            _readThread = new ReadThread(*_readBufs, _reader);
            bool pinned = (_device->_type != CPU);
            _decodeBufs = new BufferPool(_batchSize * _datumSize,
                                         _batchSize * _targetSize, pinned);
            int numCores = thread::hardware_concurrency();
            int itemsPerThread = (_batchSize - 1) /  numCores + 1;
            int threadCount =  (_batchSize - 1) / itemsPerThread + 1;
            threadCount = std::min(threadCount, _batchSize);
            _decodeThreads = new DecodeThreadPool(threadCount,
                    _batchSize, _datumSize, _targetSize,
                    *_readBufs, *_decodeBufs, _device, _media);
        } catch(std::bad_alloc&) {
            return -1;
        }
        _decodeThreads->start();
        _readThread->start();
        return 0;
    }

    void stop() {
        _readThread->stop();
        while (_readThread->stopped() == false) {
            std::this_thread::yield();
            drain();
        }
        while ((_decodeBufs->empty() == false) ||
               (_readBufs->empty() == false)) {
            drain();
        }
        _decodeThreads->stop();

        delete _readBufs;
        delete _readThread;
        delete _decodeBufs;
        delete _decodeThreads;
        _readBufs = 0;
        _readThread = 0;
        _decodeBufs = 0;
        _decodeThreads = 0;
    }

    int reset() {
        stop();
        _reader->reset();
        start();
        return 0;
    }

    void next(Buffer<char>* dataBuf, Buffer<char>* targetsBuf) {
        // Copy minibatch data into the buffers passed in.
        // Only used for testing purposes.
        {
            unique_lock<mutex> lock(_decodeBufs->getMutex());
            while (_decodeBufs->empty()) {
                _decodeBufs->waitForNonEmpty(lock);
            }
            Buffer<char>* data = _decodeBufs->getForRead().first;
            memcpy(dataBuf->_data, data->_data, dataBuf->_size);
            Buffer<char>* targets = _decodeBufs->getForRead().second;
            memcpy(targetsBuf->_data, targets->_data, targetsBuf->_size);
            _decodeBufs->advanceReadPos();
        }
        _decodeBufs->signalNonFull();
    }

    void next() {
        unique_lock<mutex> lock(_decodeBufs->getMutex());
        if (_first == true) {
            _first = false;
        } else {
            // Unlock the buffer used for the previous minibatch.
            _decodeBufs->advanceReadPos();
            _decodeBufs->signalNonFull();
        }
        while (_decodeBufs->empty()) {
            _decodeBufs->waitForNonEmpty(lock);
        }
    }

    Reader* getReader() {
        return _reader;
    }

    Media* getMedia() {
        return _media;
    }

    Device* getDevice() {
        return _device;
    }

private:
    void drain() {
        {
            unique_lock<mutex> lock(_decodeBufs->getMutex());
            if (_decodeBufs->empty() == true) {
                return;
            }
            _decodeBufs->advanceReadPos();
        }
        _decodeBufs->signalNonFull();
    }


private:
    bool                        _first;
    int                         _batchSize;
    int                         _datumSize;
    int                         _targetSize;
    BufferPool*                 _readBufs;
    BufferPool*                 _decodeBufs;
    ReadThread*                 _readThread;
    DecodeThreadPool*           _decodeThreads;
    Device*                     _device;
    Reader*                     _reader;
    Media*                      _media;
};
