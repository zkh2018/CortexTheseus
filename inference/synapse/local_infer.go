package synapse

import (
	"strings"
	"sync"

	"github.com/CortexFoundation/CortexTheseus/common/lru"
	"github.com/CortexFoundation/CortexTheseus/inference"
	"github.com/CortexFoundation/CortexTheseus/inference/synapse/kernel"
	"github.com/CortexFoundation/CortexTheseus/log"
)

const (
	DATA_PATH   string = "/data"
	SYMBOL_PATH string = "/data/symbol"
	PARAM_PATH  string = "/data/params"
)

func getReturnByStatusCode(ret interface{}, status int) (interface{}, error) {
	switch status {
	case kernel.ERROR_RUNTIME:
		return nil, KERNEL_RUNTIME_ERROR
	case kernel.ERROR_LOGIC:
		return nil, KERNEL_LOGIC_ERROR
	case kernel.SUCCEED:
		return ret, nil
	}
	log.Warn("status code invalid", "code", status)
	return nil, KERNEL_RUNTIME_ERROR
}

func (s *Synapse) getGasByInfoHash(modelInfoHash string) (gas uint64, err error) {

	if len(modelInfoHash) < 2 || !strings.HasPrefix(modelInfoHash, "0x") {
		return 0, KERNEL_RUNTIME_ERROR
	}

	var (
		modelHash     = strings.ToLower(modelInfoHash[2:])
		modelJson     []byte
		modelJson_err error
	)
	modelJson, modelJson_err = s.config.Storagefs.GetFile(modelHash, SYMBOL_PATH)
	if modelJson_err != nil || modelJson == nil {
		log.Warn("GetGasByInfoHash: get file failed", "error", modelJson_err, "hash", modelInfoHash)
		return 0, KERNEL_RUNTIME_ERROR
	}

	cacheKey := RLPHashString("estimate_ops_" + modelHash)
	if v, ok := s.simpleCache.Load(cacheKey); ok && !s.config.IsNotCache {
		log.Debug("Infer Success via Cache", "result", v.(uint64))
		return v.(uint64), nil
	}
	var status int
	gas, status = kernel.GetModelGasFromGraphFile(s.lib, modelJson)
	if _, err := getReturnByStatusCode(gas, status); err != nil {
		return 0, err
	}

	if !s.config.IsNotCache {
		s.simpleCache.Store(cacheKey, gas)
	}
	return gas, err
}

func (s *Synapse) inferByInfoHash(modelInfoHash, inputInfoHash string) (res []byte, err error) {
	if len(modelInfoHash) < 2 || len(inputInfoHash) < 2 || !strings.HasPrefix(modelInfoHash, "0x") || !strings.HasPrefix(inputInfoHash, "0x") {
		return nil, KERNEL_RUNTIME_ERROR
	}

	var (
		modelHash = strings.ToLower(modelInfoHash[2:])
		inputHash = strings.ToLower(inputInfoHash[2:])
	)

	cacheKey := RLPHashString(modelHash + "_" + inputHash)

	if _, ok := CvmFixHashes[cacheKey]; ok {
		return CvmFixHashes[cacheKey], nil
	}

	if v, ok := s.simpleCache.Load(cacheKey); ok && !s.config.IsNotCache {
		log.Debug("Infer Success via Cache", "result", v.([]byte))
		return v.([]byte), nil
	}

	inputBytes, dataErr := s.config.Storagefs.GetFile(inputHash, DATA_PATH)
	if dataErr != nil {
		log.Warn("inferByInfoHash: get file failed",
			"input hash", inputHash, "error", dataErr)
		return nil, KERNEL_RUNTIME_ERROR
	}
	reader, reader_err := inference.NewBytesReader(inputBytes)
	if reader_err != nil {
		log.Warn("inferByInfoHash: read data failed",
			"input hash", inputHash, "error", reader_err)
		return nil, KERNEL_LOGIC_ERROR
	}
	data, read_data_err := ReadData(reader)
	if read_data_err != nil {
		log.Warn("inferByInfoHash: read data failed",
			"input hash", inputHash, "error", read_data_err)
		return nil, KERNEL_LOGIC_ERROR
	}

	return s.inferByInputContent(modelInfoHash, inputInfoHash, data)
}

func (s *Synapse) inferByInputContent(modelInfoHash, inputInfoHash string, inputContent []byte) (res []byte, err error) {
	if len(modelInfoHash) < 2 || len(inputInfoHash) < 2 || !strings.HasPrefix(modelInfoHash, "0x") || !strings.HasPrefix(inputInfoHash, "0x") {
		return nil, KERNEL_RUNTIME_ERROR
	}

	var (
		modelHash = strings.ToLower(modelInfoHash[2:])
		inputHash = strings.ToLower(inputInfoHash[2:])
	)
	// Inference Cache
	cacheKey := RLPHashString(modelHash + "_" + inputHash)
	if v, ok := s.simpleCache.Load(cacheKey); ok && !s.config.IsNotCache {
		log.Debug("Infer Succeed via Cache", "result", v.([]byte))
		return v.([]byte), nil
	}

	// lazy initialization of model cache
	if _, ok := s.caches[s.config.DeviceId]; !ok {
		memoryUsage := s.config.MaxMemoryUsage
		if memoryUsage < MinMemoryUsage {
			memoryUsage = MinMemoryUsage
		}
		memoryUsage -= ReservedMemoryUsage
		s.caches[s.config.DeviceId] = lru.New(memoryUsage)
		s.caches[s.config.DeviceId].OnEvicted = func(key lru.Key, value interface{}) {
			value.(*kernel.Model).Free()
		}
	}

	var (
		result []byte
		model  *kernel.Model
		status int
	)

	v, _ := s.modelLock.LoadOrStore(modelHash, sync.Mutex{})
	mutex := v.(sync.Mutex)
	mutex.Lock()
	defer mutex.Unlock()

	model_tmp, has_model := s.caches[s.config.DeviceId].Get(modelHash)
	if !has_model {
		modelJson, modelJson_err := s.config.Storagefs.GetFile(modelHash, SYMBOL_PATH)
		if modelJson_err != nil || modelJson == nil {
			log.Warn("inferByInputContent: model loaded failed",
				"model hash", modelHash, "error", modelJson_err)
			return nil, KERNEL_RUNTIME_ERROR
		}
		modelParams, modelParams_err := s.config.Storagefs.GetFile(modelHash, PARAM_PATH)
		if modelParams_err != nil || modelParams == nil {
			log.Warn("inferByInputContent: params loaded failed",
				"model hash", modelHash, "error", modelParams_err)
			return nil, KERNEL_RUNTIME_ERROR
		}
		var deviceType = 0
		if s.config.DeviceType == "cuda" {
			deviceType = 1
		}
		model, status = kernel.New(s.lib, modelJson, modelParams, deviceType, s.config.DeviceId)
		// TODO(wlt): all returned runtime_error
		if _, err := getReturnByStatusCode(model, status); err != nil {
			return nil, KERNEL_RUNTIME_ERROR
		}
		s.caches[s.config.DeviceId].Add(modelHash, model, int64(model.Size()))
	} else {
		model = model_tmp.(*kernel.Model)
	}

	result, status = model.Predict(inputContent)
	// TODO(wlt): all returned runtime_error
	if _, err := getReturnByStatusCode(result, status); err != nil {
		return nil, KERNEL_RUNTIME_ERROR
	}

	if !s.config.IsNotCache {
		s.simpleCache.Store(cacheKey, result)
	}

	return result, nil
}

func (s *Synapse) Available(infoHash string, rawSize int64) error {
	if s.config.IsRemoteInfer {
		errRes := s.remoteAvailable(
			infoHash,
			rawSize,
			s.config.InferURI)
		return errRes
	}
	is_ok, err := s.config.Storagefs.Available(infoHash, rawSize)
	if err != nil {
		log.Debug("File verification failed", "infoHash", infoHash, "error", err)
		return KERNEL_RUNTIME_ERROR
	} else if !is_ok {
		log.Warn("File is unavailable",
			"info hash", infoHash, "error", KERNEL_LOGIC_ERROR)
		return KERNEL_LOGIC_ERROR
	}
	log.Debug("File available", "info hash", infoHash)
	return nil
}
