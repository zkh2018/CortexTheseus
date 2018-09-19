package infernet

/*
#cgo LDFLAGS: -L./ -lcortexnet
#cgo CFLAGS: -I./src

#include "interface.h"
*/
import "C"
import (
	"errors"
	"fmt"
	"unsafe"
)

func readImg(input string) ([]byte, error) {
	r, rerr := NewFileReader(input)
	if rerr != nil {
		fmt.Println("Error: ", rerr)
		return nil, rerr
	}

	// Infer data must between [0, 127)
	data, derr := r.GetBytes()
	for i, v := range data {
		data[i] = uint8(v) / 2
	}
	// DumpToFile("tmp.dump", data)
	if derr != nil {
		return nil, derr
	}

	return data, nil
}

func InferCore(modelCfg, modelBin, image string) (uint64, error) {

	net := C.load_model(
		C.CString(modelCfg),
		C.CString(modelBin))

	resLen := int(C.get_output_length(net))
	if resLen == 0 {
		return 0, errors.New("Model result len is 0")
	}

	res := make([]byte, resLen)

	imageData, rerr := readImg(image)
	if rerr != nil {
		return 0, rerr
	}

	flag := C.predict(
		net,
		(*C.char)(unsafe.Pointer(&imageData[0])),
		(*C.char)(unsafe.Pointer(&res[0])))

	if flag != 0 {
		return 0, errors.New("Predict Error")
	}

	// Find the maximum possibility of label
	max := int8(res[0])
	label := uint64(0)
	for idx := 1; idx < resLen; idx++ {
		if int8(res[idx]) > max {
			max = int8(res[idx])
			label = uint64(idx)
		}
	}

	C.free_model(net)

	return label, nil
}

func main() {
	label, err := InferCore("./infer_data/model/param", "./infer_data/model/symbol", "./infer_data/image/data")

	fmt.Println(label, err)

	// readImg("./img.8b")
	// fmt.Println("Result: " + res)
	// fmt.Println("Result: " + res + " Length: " + string(len(res)))
}