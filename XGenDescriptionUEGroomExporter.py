# %%
import alembic.Abc as abc
import alembic.AbcGeom as abcGeom
import alembic.AbcCoreAbstract as abcA
import maya.OpenMaya as om1
import maya.api.OpenMayaAnim as omAnim
import maya.api.OpenMaya as om
import imath
import array
import zlib
import json
import maya.cmds as cmds
from typing import List
import time
import struct
import uuid
import xgenm as xg
import os

_XGenExporterVersion = "1.08"
print_debug = False


# %%
def list2ImathArray(l: list, _type):
    arr = _type(len(l))
    for i in range(len(l)):
        arr[i] = l[i]
    return arr


def floatList2V3fArray(l: list):
    arr = imath.V3fArray(len(l) // 3)
    for i in range(len(arr)):
        arr[i].x = l[i * 3]
        arr[i].y = l[i * 3 + 1]
        arr[i].z = l[i * 3 + 2]
    return arr


# %%
def getXgenData(fnDepNode: om.MFnDependencyNode, keys):
    splineData: om.MPlug = fnDepNode.findPlug("outSplineData", False)

    handle: om.MDataHandle = splineData.asMObject()
    mdata = om.MFnPluginData(handle)
    mData = mdata.data()

    rawData = mData.writeBinary()

    def GetBlocks(bype_data):
        address = 0
        i = 0
        blocks = []
        maxIt = 100
        while address < len(bype_data) - 1:
            size = int.from_bytes(bype_data[address + 8:address + 16], byteorder='little', signed=False)
            type_code = int.from_bytes(bype_data[address:address + 4], byteorder='little', signed=False)
            blocks.append((address + 16, address + 16 + size, type_code))
            address += size + 16
            i += 1
            if i > maxIt:
                break
        return blocks

    dataBlocks = GetBlocks(rawData)
    headerBlock = dataBlocks[0]
    dataBlocks.pop(0)

    dataJson = json.loads(rawData[headerBlock[0]:headerBlock[1]])
    # print(dataJson)
    Header = dataJson['Header']

    Items = dict()

    def readItems(items):
        for k, v in items:
            if isinstance(v, int):
                group = v >> 32
                index = v & 0xFFFFFFFF
                addr = (group, index)
                if k not in Items:
                    Items[k] = [addr]
                else:
                    Items[k].append(addr)

    for i in range(len(dataJson['Items'])):
        readItems(dataJson['Items'][i].items())
    for i in range(len(dataJson['RefMeshArray'])):
        readItems(dataJson['RefMeshArray'][i].items())

    # print(Items)
    decompressedData = dict()

    def decompressData(group, index):
        if group not in decompressedData:
            if Header['GroupBase64']:
                raise Exception("我还没有碰到Base64的情况，请提醒我更新代码")
            if Header['GroupDeflate']:
                validData = zlib.decompress(rawData[dataBlocks[group][0] + 32:])
            else:
                validData = rawData[dataBlocks[group][0]:dataBlocks[group][1]]
            decompressedData[group] = validData
        else:
            validData = decompressedData[group]
        blocks = GetBlocks(validData)
        return validData[blocks[index][0]:blocks[index][1]]

    outputs = {key: [] for key in keys}
    for k, v in Items.items():
        if k not in outputs:
            continue
        if k == 'PrimitiveInfos':
            dtype_format = '<IQ'
            for addr in v:
                decompressed_data = decompressData(*addr)
                PrimitiveInfos = []
                record_size = struct.calcsize(dtype_format)
                for i in range(0, len(decompressed_data), record_size):
                    PrimitiveInfo = struct.unpack_from(f'{dtype_format}', decompressed_data, i)
                    PrimitiveInfos.append(PrimitiveInfo)
                outputs[k].append(PrimitiveInfos)
        elif k in ('FaceUV', 'Positions', 'WIDTH_CV'):
            for addr in v:
                decompressed_data = decompressData(*addr)
                outputs[k].append(array.array('f', decompressed_data))
        elif k == 'FaceId':
            for addr in v:
                decompressed_data = decompressData(*addr)
                outputs[k].append(array.array('i', decompressed_data))
    return [outputs[k] for k in keys]


# %%
class AbcType:
    string = (abcGeom.OStringGeomParam, abcGeom.OStringGeomParamSample)
    int16 = (abcGeom.OInt16GeomParam, abcGeom.OInt16GeomParamSample)
    int32 = (abcGeom.OInt32GeomParam, abcGeom.OInt32GeomParamSample)
    int64 = (abcGeom.OInt64GeomParam, abcGeom.OInt64GeomParamSample)
    color3f = (abcGeom.OC3fGeomParam, abcGeom.OC3fGeomParamSample)
    float = (abcGeom.OFloatGeomParam, abcGeom.OFloatGeomParamSample)
    vector2f = (abcGeom.OV2fGeomParam, abcGeom.OV2fGeomParamSample)
    vector3f = (abcGeom.OV3fGeomParam, abcGeom.OV3fGeomParamSample)


# %%
class CurvesProxy:
    def __init__(self, curveObj: abcGeom.OCurves, fnDepNode: om.MFnDependencyNode, needRootList=False, animation=False):
        self.hairRootList = None
        self.schema: abcGeom.OCurvesSchema = curveObj.getSchema()
        self.cp: abc.OCompoundProperty = self.schema.getArbGeomParams()
        self.needRootList = needRootList
        self.animation = animation
        self.firstSamp = abcGeom.OCurvesSchemaSample()
        self.fnDepNode = fnDepNode
        self.curves = None
        self.groupName = None
        self.is_guide = False
        self.needBakeUV = False

    def write_param(self, name: str, abcType, data, scope=abcGeom.GeometryScope.kUniformScope, extent=1):
        if len(data) == 1:
            scope = abcGeom.GeometryScope.kConstantScope
        param = abcType[0](self.cp, name, False, scope, extent)
        sample = abcType[1](data, scope)
        param.set(sample)

    def write_group_name(self, group_name: str, write_card_id=False):
        group_name_data = list2ImathArray([group_name], imath.StringArray)
        self.write_param('groom_group_name', AbcType.string, group_name_data)
        if write_card_id:
            self.write_param('groom_group_cards_id', AbcType.string, group_name_data)
        self.groupName = group_name

    def write_is_guide(self, is_guide=True):
        self.isGuide = is_guide
        if is_guide:
            self.write_param('groom_guide', AbcType.int16, list2ImathArray([1], imath.ShortArray))

    def write_group_id(self, group_id: int):
        self.write_param('groom_group_id', AbcType.int32, list2ImathArray([group_id], imath.IntArray))

    def write_first_frame(self):
        itDag = om.MItDag()
        itDag.reset(self.fnDepNode.object(), om.MItDag.kDepthFirst, om.MFn.kCurve)
        curves = []
        while not itDag.isDone():
            curve_node = itDag.currentItem()
            curves.append(curve_node)
            itDag.next()
        self.curves = curves

        numCurves = len(self.curves)
        if numCurves == 0:
            return

        curve = om.MFnNurbsCurve(self.curves[0])

        orders = imath.IntArray(numCurves)
        nVertices = imath.IntArray(numCurves)
        pointslist = []
        knots = []
        if self.needRootList:
            self.hairRootList = []

        samp = self.firstSamp
        samp.setBasis(abcGeom.BasisType.kBsplineBasis)
        samp.setWrap(abcGeom.CurvePeriodicity.kNonPeriodic)

        if curve.degree == 3:
            samp.setType(abcGeom.CurveType.kCubic)
        elif curve.degree == 1:
            samp.setType(abcGeom.CurveType.kLinear)
        else:
            # samp.setType(abcGeom.CurveType.kVariableOrder)
            samp.setType(abcGeom.CurveType.kLinear)
            pass
        for i in range(numCurves):
            curve = curve.setObject(self.curves[i])
            numCVs = curve.numCVs
            orders[i] = curve.degree + 1
            nVertices[i] = numCVs
            cvArray = curve.cvPositions()
            for j in range(numCVs):
                pointslist.append(cvArray[j].x)
                pointslist.append(cvArray[j].y)
                pointslist.append(cvArray[j].z)
            if self.needRootList:
                self.hairRootList.append(om.MPoint(cvArray[0]))
            knotsArray = curve.knots()
            if len(knotsArray) > 1:
                knotsLength = len(knotsArray)
                if (knotsArray[0] == knotsArray[knotsLength - 1] or
                        knotsArray[0] == knotsArray[1]):
                    knots.append(float(knotsArray[0]))
                else:
                    knots.append(float(2 * knotsArray[0] - knotsArray[1]))

                for j in range(knotsLength):
                    knots.append(float(knotsArray[j]))

                if (knotsArray[0] == knotsArray[knotsLength - 1] or
                        knotsArray[knotsLength - 1] == knotsArray[knotsLength - 2]):
                    knots.append(float(knotsArray[knotsLength - 1]))
                else:
                    knots.append(float(2 * knotsArray[knotsLength - 1] - knotsArray[knotsLength - 2]))
        samp.setCurvesNumVertices(nVertices)
        samp.setPositions(floatList2V3fArray(pointslist))
        samp.setOrders(list2ImathArray(orders, imath.UnsignedCharArray))
        samp.setKnots(list2ImathArray(knots, imath.FloatArray))

        # widths = list2ImathArray([0.1], imath.FloatArray)
        # widths = abc.Float32TPTraits()
        # widths = abcGeom.OFloatGeomParamSample(widths, abcGeom.GeometryScope.kConstantScope)
        # samp.setWidths(widths)
        self.schema.set(samp)

    def write_frame(self):
        numCurves = len(self.curves)
        if numCurves == 0:
            return
        curve = om.MFnNurbsCurve(self.curves[0])

        samp = abcGeom.OCurvesSchemaSample()
        samp.setBasis(self.firstSamp.getBasis())
        samp.setWrap(self.firstSamp.getWrap())
        samp.setType(self.firstSamp.getType())
        samp.setCurvesNumVertices(self.firstSamp.getCurvesNumVertices())
        samp.setOrders(self.firstSamp.getOrders())
        samp.setKnots(self.firstSamp.getKnots())

        pointslist = []
        for i in range(numCurves):
            curve = curve.setObject(self.curves[i])
            numCVs = curve.numCVs
            cvArray = curve.cvPositions()
            for j in range(numCVs):
                pointslist.append(cvArray[j].x)
                pointslist.append(cvArray[j].y)
                pointslist.append(cvArray[j].z)

        samp.setPositions(floatList2V3fArray(pointslist))

        self.schema.set(samp)

    def bake_uv(self, bakeMesh: om.MFnMesh, uv_set: str = None):
        if not self.needBakeUV or self.hairRootList is None:
            return
        if bakeMesh is None:
            return
        if uv_set is None:
            uv_set = bakeMesh.currentUVSetName()
        elif uv_set not in bakeMesh.getUVSetNames():
            raise Exception(f'Invalid UV Set : {uv_set}')

        uvs = imath.V2fArray(len(self.hairRootList))
        for i, hairRoot in enumerate(self.hairRootList):
            res = bakeMesh.getUVAtPoint(hairRoot, om.MSpace.kWorld, uvSet=uv_set)
            uvs[i].x = res[0]
            uvs[i].y = res[1]

        self.write_param('groom_root_uv', AbcType.vector2f, uvs)


# %%
try:
    from PySide6 import QtCore, QtWidgets, QtGui
    import shiboken6 as shiboken
except:
    from PySide2 import QtCore, QtWidgets, QtGui
    import shiboken2 as shiboken

import maya.OpenMayaUI as om1ui


def mayaWindow():
    main_window_ptr = om1ui.MQtUtil.mainWindow()
    return shiboken.wrapInstance(int(main_window_ptr), QtWidgets.QWidget)


class FileSelectorWidget(QtWidgets.QWidget):
    def __init__(self, callback):
        super(FileSelectorWidget, self).__init__()
        self.callback = callback
        self.setup_ui()

    def setup_ui(self):
        self.layout = QtWidgets.QHBoxLayout(self)
        self.file_line_edit = QtWidgets.QLineEdit(self)
        self.layout.addWidget(self.file_line_edit)
        self.browse_button = QtWidgets.QPushButton("Browse", self)
        self.layout.addWidget(self.browse_button)
        self.browse_button.clicked.connect(self.browse_file)
        self.file_line_edit.textChanged.connect(self.callback)

    def browse_file(self):
        file_path = cmds.fileDialog2(fileMode=1, caption="Select a File")
        if file_path:
            self.file_line_edit.setText(file_path[0])

    def set_file_path(self, path):
        return self.file_line_edit.setText(path)

    def get_file_path(self):
        return self.file_line_edit.text()


# %%
SaveXGenDesWindowParentName = "_saveXGenDesWindow"


def getSaveXGenDesWindowParent():
    sel = om.MSelectionList()
    try:
        sel.add(SaveXGenDesWindowParentName)
    except:
        trans = om.MFnTransform()
        trans.create()
        trans.setName(SaveXGenDesWindowParentName)
        sel.add(SaveXGenDesWindowParentName)
    return sel.getDagPath(0)


def deleteSaveXGenDesWindowParent():
    if cmds.objExists(SaveXGenDesWindowParentName):
        cmds.delete(SaveXGenDesWindowParentName)


# %%


def getExpressionPath(expr: str, pal_path, des_path, fx_name):
    expr = expr.replace('${DESC}', xg.descriptionPath(pal_path, des_path)).replace('${FXMODULE}', fx_name)
    ptex_path = None
    if os.path.isdir(expr):
        for f in os.listdir(expr):
            if f.endswith('.ptx'):
                ptex_path = f

    if ptex_path is None:
        return ""
    return os.path.normpath(os.path.join(expr, ptex_path))


def getClumpingPtexPath(dn: om.MFnDependencyNode):
    if not dn.object().hasFn(om.MFn.kTransform):
        des_obj = om.MFnDagNode(dn.object()).parent(0)
    else:
        des_obj = dn.object()
    des_path = str(om.MDagPath.getAPathTo(des_obj))
    pal_path = str(om.MDagPath.getAPathTo(om.MFnDagNode(des_obj).parent(0)))
    clumping = None
    for fx in xg.fxModules(pal_path, des_path):
        if fx.startswith('Clumping'):
            clumping = fx
            break
    if clumping is None:
        return ""
    expr = xg.getAttr("mapDir", pal_path, des_path, clumping)
    return getExpressionPath(expr, pal_path, des_path, clumping)


# %%
def generate_short_hash():
    unique_id = uuid.uuid4()
    return str(unique_id).replace('-', '')[:8]


def ConvertToInteractive(dn: om.MFnDependencyNode):
    if not dn.object().hasFn(om.MFn.kTransform):
        path = om.MDagPath.getAPathTo(om.MFnDagNode(dn.object()).parent(0))
    else:
        path = om.MDagPath.getAPathTo(dn.object())
    cmds.select(path, replace=True)
    prefix = "z" + generate_short_hash()
    res = cmds.xgmGroomConvert(prefix=prefix)
    if res is None or len(res) == 0:
        raise Exception("Convert to interactive failed.")
    sel: om.MSelectionList = om.MGlobal.getActiveSelectionList()

    if not res[0].startswith(prefix):
        fnDepNode = om.MFnDependencyNode(sel.getDependNode(0))
        if not fnDepNode.object().hasFn(om.MFn.kTransform):
            fnDepNode = om.MFnDependencyNode(om.MFnDagNode(fnDepNode.object()).parent(0))
        fnDepNode.setName('_'.join((prefix, fnDepNode.name())))

    spline = om.MFnDagNode(sel.getDagPath(0))
    om.MFnDagNode(getSaveXGenDesWindowParent()).addChild(spline.parent(0))
    return spline
    # return curve.parent(0)


# %%
import ctypes
from ctypes import c_void_p, c_uint64, c_ulonglong, c_float, c_int, c_char_p

PtexSamplerDllFuncName = "_PtexSamplerDllFunc"


class PtexSampler:
    class DllFunc:
        @staticmethod
        def getVFunc(obj, index, *args):
            vtble = ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p)).contents
            vfuncAddr = ctypes.cast(vtble.value + index * 8, ctypes.POINTER(ctypes.c_void_p)).contents.value
            return ctypes.CFUNCTYPE(*args)(vfuncAddr)

        @staticmethod
        def getPtexFilterEvelFunc(filter):
            ptexFilterEvel = PtexSampler.DllFunc.getVFunc(filter, 2, c_void_p, c_void_p, c_void_p, c_int, c_int, c_int,
                                                          c_float, c_float,
                                                          c_float, c_float, c_float, c_float)
            return ptexFilterEvel

        @staticmethod
        def getPtexTextureReleaseFunc(ptexTexture):
            ptexTextureRelease = PtexSampler.DllFunc.getVFunc(ptexTexture, 1, c_void_p)
            return ptexTextureRelease

        class MyVector(ctypes.Structure):
            _fields_ = [
                ("_Myfirst", ctypes.POINTER(ctypes.c_float)),  # pointer to beginning of array
                ("_Mylast", ctypes.POINTER(ctypes.c_float)),  # pointer to current end of sequence
                ("_Myend", ctypes.POINTER(ctypes.c_float))  # pointer to end of sequence
            ]

            def __init__(self, size=10):
                array = (ctypes.c_float * size)()
                # 为每个指针分配内存
                self._Myfirst = ctypes.cast(array, ctypes.POINTER(ctypes.c_float))
                # _Mylast 初始化为数组的开始（指向和 _Myfirst 相同的位置）
                self._Mylast = self._Myfirst  # 当前结束位置指向数组的开始
                _myend_address = ctypes.addressof(array) + ctypes.sizeof(array)  # 获取数组末尾（地址）
                self._Myend = ctypes.cast(_myend_address, ctypes.POINTER(ctypes.c_float))

            def __getitem__(self, index):
                return self._Myfirst[index]

    def close(self):
        getPtexTextureRelease = PtexSampler.DllFunc.getPtexTextureReleaseFunc(self.ptexTexture)
        getPtexTextureRelease(self.ptexTexture)

    def setupFilter(self, path):
        DllFunc: PtexSampler.DllFunc = globals()[PtexSamplerDllFuncName]
        err = ctypes.c_uint64()
        ptexTexture = DllFunc.ptex_open(
            path.encode(),
            ctypes.byref(err), 0)
        if ptexTexture is None:
            raise Exception("no ptexTexture found")
        self.ptexTexture = ctypes.c_void_p(ptexTexture)

        # 定义Options结构体
        class Options(ctypes.Structure):
            _fields_ = [
                ("__structSize", ctypes.c_int),  # (for internal use only)
                ("filter", ctypes.c_int),  # Filter type.
                ("lerp", ctypes.c_bool),  # Interpolate between mipmap levels.
                ("sharpness", ctypes.c_float),  # Filter sharpness, 0..1 (for general bi-cubic filter only).
                ("noedgeblend", ctypes.c_bool)  # Disable cross-face filtering.
            ]

            def __init__(self, filter_=0, lerp_=False, sharpness_=0.0,
                         noedgeblend_=False):  # Point-sampled (no filtering)
                self.__structSize = ctypes.sizeof(Options)  # 设置结构体大小
                self.filter = filter_  # 设置过滤器类型
                self.lerp = lerp_  # 设置是否插值
                self.sharpness = sharpness_  # 设置过滤器锐度
                self.noedgeblend = noedgeblend_  # 设置是否禁用跨面过滤

        options = Options()
        filter = DllFunc.ptex_getFilter(self.ptexTexture, options)
        filter = ctypes.cast(filter, ctypes.c_void_p)
        self.ptexFilterEvalFunc = DllFunc.getPtexFilterEvelFunc(filter)
        self.vector = DllFunc.temp_vector
        self.filter = filter

    def __init__(self, path: str):
        self.path = path
        if PtexSamplerDllFuncName in globals():
            self.setupFilter(path)
            return

        DllFunc = PtexSampler.DllFunc()
        globals()[PtexSamplerDllFuncName] = DllFunc

        ptex_dll = ctypes.cdll.LoadLibrary("Ptex.dll")
        versions = ['2_2', '2_3', '2_4', '2_5', '2_6']
        version = None
        for _v in versions:
            try:
                Ptex_String_release = ptex_dll[f"??1String@v{_v}@Ptex@@QEAA@XZ"]
                version = _v
                break
            except:
                pass
        if version is None:
            raise Exception("Could not find correct Ptex version")

        DllFunc.ptex_open = ptex_dll[f"?open@PtexTexture@v{version}@Ptex@@SAPEAV123@PEBDAEAVString@23@_N@Z"]
        DllFunc.ptex_open.restype = ctypes.c_void_p
        DllFunc.ptex_getFilter = ptex_dll[
            f'?getFilter@PtexFilter@v{version}@Ptex@@SAPEAV123@PEAVPtexTexture@23@AEBUOptions@123@@Z']
        DllFunc.ptex_getFilter.restype = ctypes.c_void_p
        DllFunc.temp_vector = DllFunc.MyVector(10)
        self.setupFilter(path)

    def sampleData(self, faceU, faceV, faceId):
        self.ptexFilterEvalFunc(self.filter, self.vector._Myfirst, 0, 3, faceId, faceU, faceV, 0, 0, 0, 0)
        return self.vector[:3]


# %%
class XGenProxyEveryFrame(CurvesProxy):
    def __init__(self, curveObj: abcGeom.OCurves, descFnDepNode: om.MFnDependencyNode, needRootList=False,
                 animation=False):
        super().__init__(curveObj, None, needRootList, animation)
        self.descFnDepNode = descFnDepNode
        self.order_offset_map = None

    def write_first_frame(self):
        if print_debug:
            startTime = time.time()

        spline = ConvertToInteractive(self.descFnDepNode)
        self.fnDepNode = spline
        self.firstSpline = spline
        PrimitiveInfosList, PositionsDataList, WidthsDataList, FaceIdList, FaceUVList = getXgenData(self.fnDepNode,
                                                                                                    ('PrimitiveInfos',
                                                                                                     'Positions',
                                                                                                     'WIDTH_CV',
                                                                                                     'FaceId',
                                                                                                     'FaceUV'))
        if print_debug:
            print("getXgenData: %.4f" % (time.time() - startTime))
            startTime = time.time()
        numCurves = 0
        numCVs = 0
        for i, PrimitiveInfos in enumerate(PrimitiveInfosList):
            numCurves += len(PrimitiveInfos)
            for PrimitiveInfo in PrimitiveInfos:
                numCVs += PrimitiveInfo[1]
        self.numCurves = numCurves
        self.numCVs = numCVs
        orders = imath.UnsignedCharArray(numCurves)
        nVertices = imath.IntArray(numCurves)

        samp = self.firstSamp
        samp.setBasis(abcGeom.BasisType.kBsplineBasis)
        samp.setWrap(abcGeom.CurvePeriodicity.kNonPeriodic)
        samp.setType(abcGeom.CurveType.kCubic)

        degree = 3
        pointArray = imath.V3fArray(numCVs)
        widthArray = imath.FloatArray(numCVs)
        if self.needRootList:
            self.hairRootList = []
        knots = []

        curveIndex = 0
        cvIndex = 0
        cvOffsets = imath.IntArray(numCurves)
        for j in range(len(PrimitiveInfosList)):
            PrimitiveInfos = PrimitiveInfosList[j]
            posData = PositionsDataList[j]
            widthData = WidthsDataList[j]
            for i, PrimitiveInfo in enumerate(PrimitiveInfos):
                offset = PrimitiveInfo[0]
                length = int(PrimitiveInfo[1])
                if length < 2:
                    continue
                startAddr = offset * 3
                cvOffsets[curveIndex] = cvIndex
                for k in range(length):
                    pointArray[cvIndex].x = posData[startAddr]
                    pointArray[cvIndex].y = posData[startAddr + 1]
                    pointArray[cvIndex].z = posData[startAddr + 2]
                    if k == 0 and self.needRootList:
                        self.hairRootList.append(om.MPoint(pointArray[cvIndex]))
                    widthArray[cvIndex] = widthData[offset + k]
                    startAddr += 3
                    cvIndex += 1

                orders[curveIndex] = degree + 1
                nVertices[curveIndex] = length

                knotsInsideNum = length - degree + 1
                knotsList = [*([0] * degree), *list(range(knotsInsideNum)),
                             *([knotsInsideNum - 1] * degree)]  # The endpoint repeats one more than Maya
                # print(knotsList)
                knots += knotsList
                curveIndex += 1

        samp.setCurvesNumVertices(nVertices)
        samp.setPositions(pointArray)
        samp.setKnots(list2ImathArray(knots, imath.FloatArray))
        samp.setOrders(orders)

        widths = abcGeom.OFloatGeomParamSample(widthArray, abcGeom.GeometryScope.kVertexScope)
        samp.setWidths(widths)
        self.schema.set(samp)
        if self.animation:
            index2order = self.get_index2order(FaceIdList, FaceUVList)
            self.order_offset_map = imath.IntArray(numCurves)
            for i, offset in zip(index2order, cvOffsets):
                self.order_offset_map[i] = offset
        if print_debug:
            # print(self.order_offset_map)
            print("write_first_frame: %.4f" % (time.time() - startTime))

    @staticmethod
    def get_index2order(FaceIdList, FaceUVList):
        order_list = []
        for j in range(len(FaceIdList)):
            FaceUVData = FaceUVList[j]
            FaceIdData = FaceIdList[j]
            for i, faceId in enumerate(FaceIdData):
                u = FaceUVData[i * 2]
                v = FaceUVData[i * 2 + 1]
                order_list.append((faceId, u, v))
        sorted_list = sorted((key, i) for i, key in enumerate(order_list))
        index_list = imath.IntArray(len(order_list))
        for order_index, item in enumerate(sorted_list):
            my_index = item[1]
            index_list[my_index] = order_index
        # if print_debug:
        #     print(sorted_list)
        return index_list

    def write_frame(self):
        if print_debug:
            startTime = time.time()
        spline = ConvertToInteractive(self.descFnDepNode)
        self.fnDepNode = spline
        PrimitiveInfosList, PositionsDataList, FaceIdList, FaceUVList = getXgenData(self.fnDepNode, ('PrimitiveInfos',
                                                                                                     'Positions',
                                                                                                     'FaceId',
                                                                                                     'FaceUV'))

        numCVs = self.numCVs

        samp = abcGeom.OCurvesSchemaSample()
        samp.setBasis(self.firstSamp.getBasis())
        samp.setWrap(self.firstSamp.getWrap())
        samp.setType(self.firstSamp.getType())

        samp.setCurvesNumVertices(self.firstSamp.getCurvesNumVertices())
        samp.setKnots(self.firstSamp.getKnots())
        samp.setOrders(self.firstSamp.getOrders())
        samp.setWidths(self.firstSamp.getWidths())

        if print_debug:
            s = time.time()
        index2order = self.get_index2order(FaceIdList, FaceUVList)

        pointArray = imath.V3fArray(numCVs)

        curveIndex = 0
        for j in range(len(PrimitiveInfosList)):
            PrimitiveInfos = PrimitiveInfosList[j]
            posData = PositionsDataList[j]
            for PrimitiveInfo in PrimitiveInfos:
                offset = PrimitiveInfo[0]
                length = int(PrimitiveInfo[1])
                if length < 2:
                    continue
                startAddr = offset * 3
                cvIndex = self.order_offset_map[index2order[curveIndex]]
                for k in range(length):
                    pointArray[cvIndex].x = posData[startAddr]
                    pointArray[cvIndex].y = posData[startAddr + 1]
                    pointArray[cvIndex].z = posData[startAddr + 2]
                    startAddr += 3
                    cvIndex += 1

                curveIndex += 1
        if print_debug:
            print("loop: %.4f" % (time.time() - s))
        samp.setPositions(pointArray)

        self.schema.set(samp)
        if print_debug:
            print("write_frame: %.4f" % (time.time() - startTime))


# %%

GroomGuideIdStartIndexName = '_GroomGuideIdStartIndexName'


def getGroomGuideIdStartIndex():
    return globals()[GroomGuideIdStartIndexName]


def setGroomGuideIdStartIndex(value=0):
    globals()[GroomGuideIdStartIndexName] = value


# %%
class GuideProxy(CurvesProxy):
    def __init__(self, curveObj: abcGeom.OCurves, fnDepNode: om.MFnDependencyNode, needRootList=False, animation=False):
        if not fnDepNode.object().hasFn(om.MFn.kTransform):
            fnDepNode = om.MFnDependencyNode(om.MFnDagNode(fnDepNode.object()).parent(0))
        super().__init__(curveObj, fnDepNode, needRootList, animation)
        itDag = om.MItDag()
        itDag.reset(self.fnDepNode.object(), om.MItDag.kBreadthFirst, om.MFn.kInvalid)
        guides = []
        while not itDag.isDone():
            dn = om.MFnDependencyNode(itDag.currentItem())
            if dn.typeName == 'xgmSplineGuide':
                guides.append(itDag.getPath())
            itDag.next()
        self.guides: list[om.MDagPath] = guides
        self.xgenProxy = None
        self.ptexPath = None
        self.writePtexGuideId = False

    def set_xgen_proxy_and_ptex(self, xgenSpline: XGenProxyEveryFrame, ptexPath: str):
        self.xgenProxy = xgenSpline
        self.ptexPath = ptexPath

    def write_guide_id_from_ptex(self):
        if not self.writePtexGuideId:
            return
        ptexPath = self.ptexPath
        xgenSpline = self.xgenProxy
        if ptexPath is None or ptexPath == "":
            return
        if self.xgenProxy == None:
            return
        ptexSampler = PtexSampler(ptexPath)
        self.regionPtex = ptexPath

        guide_map = dict()

        def color2Int(color):
            a = int(color[0] * 255) & 0xff
            b = int(color[1] * 255) & 0xff
            c = int(color[2] * 255) & 0xff
            return (a << 16) | (b << 8) | c

        for i, guide in enumerate(self.guides):
            dn = om.MFnDependencyNode(guide.node())
            u = dn.findPlug('uLoc', False).asFloat()
            v = dn.findPlug('vLoc', False).asFloat()
            faceId = dn.findPlug('faceId', False).asInt()
            color = ptexSampler.sampleData(u, v, faceId)
            hash = color2Int(color)
            if hash in guide_map:
                old_guide = om.MFnDependencyNode(guide_map[hash][0].node())
                print(
                    f"guide {dn.name()} and {old_guide.name()} are in the same area on texture, only use {old_guide.name()}.")
                continue
            guide_map[hash] = (guide, i)

        FaceUVList, FaceIdList = getXgenData(xgenSpline.firstSpline, ('FaceUV', 'FaceId'))

        guideIdStartIndex = getGroomGuideIdStartIndex()
        guideIdNextStartIndex = guideIdStartIndex + len(self.guides)
        groom_id_data = list2ImathArray(list(range(guideIdStartIndex, guideIdNextStartIndex)), imath.IntArray)
        self.write_param("groom_id", AbcType.int32, groom_id_data)

        spline_num = len(xgenSpline.hairRootList)
        weight_data = list2ImathArray([1.0] * spline_num, imath.FloatArray)
        xgenSpline.write_param("groom_guide_weights", AbcType.float, weight_data)

        guide_id_data = imath.IntArray(spline_num)
        first_guide_name = om.MFnDependencyNode(self.guides[0].node()).name()
        spline_index = 0
        for j in range(len(FaceIdList)):
            FaceUVData = FaceUVList[j]
            FaceIdData = FaceIdList[j]
            # print(len(FaceIdData),len(FaceUVData))
            for i, faceId in enumerate(FaceIdData):
                u = FaceUVData[i * 2]
                v = FaceUVData[i * 2 + 1]
                color = ptexSampler.sampleData(u, v, faceId)
                hash = color2Int(color)
                guide_id = guideIdStartIndex
                if hash not in guide_map:
                    print(f"The spline index ({j} ,{i}) does not have a valid guide attached to {first_guide_name}.")
                else:
                    guide_id += guide_map[hash][1]

                if spline_index >= spline_num:
                    raise Exception("spline_index >= spline_num")
                guide_id_data[spline_index] = guide_id
                # guide_map[hash][2].append(spline_index)
                spline_index += 1
        # print(guide_map)
        ptexSampler.close()
        xgenSpline.write_param("groom_closest_guides", AbcType.int32, guide_id_data)

        setGroomGuideIdStartIndex(guideIdNextStartIndex)

    def write_first_frame(self):
        numCurves = len(self.guides)
        orders = imath.IntArray(numCurves)
        nVertices = imath.IntArray(numCurves)
        pointslist = []
        knots = []
        if self.needRootList:
            self.hairRootList = []

        samp = self.firstSamp
        samp.setBasis(abcGeom.BasisType.kBsplineBasis)
        samp.setWrap(abcGeom.CurvePeriodicity.kNonPeriodic)
        samp.setType(abcGeom.CurveType.kLinear)
        degree = 1

        for i in range(numCurves):
            data = cmds.xgmGuideGeom(guide=self.guides[i], numVertices=True)
            numCVs = int(data[0])
            data = cmds.xgmGuideGeom(guide=self.guides[i], controlPoints=True)
            pointslist += data
            orders[i] = degree + 1
            nVertices[i] = numCVs
            if self.needRootList:
                self.hairRootList.append(om.MPoint(data[:3]))

            knotsInsideNum = numCVs - degree + 1
            knotsList = [*([0] * degree), *list(range(knotsInsideNum)),
                         *([knotsInsideNum - 1] * degree)]  # The endpoint repeats one more than Maya
            # print(knotsList)
            knots += knotsList
        samp.setCurvesNumVertices(nVertices)
        samp.setPositions(floatList2V3fArray(pointslist))
        samp.setOrders(list2ImathArray(orders, imath.UnsignedCharArray))
        samp.setKnots(list2ImathArray(knots, imath.FloatArray))
        self.schema.set(samp)

    def write_frame(self):
        numCurves = len(self.guides)
        if numCurves == 0:
            return

        samp = abcGeom.OCurvesSchemaSample()
        samp.setBasis(self.firstSamp.getBasis())
        samp.setWrap(self.firstSamp.getWrap())
        samp.setType(self.firstSamp.getType())
        samp.setCurvesNumVertices(self.firstSamp.getCurvesNumVertices())
        samp.setOrders(self.firstSamp.getOrders())
        samp.setKnots(self.firstSamp.getKnots())

        pointslist = []
        for i in range(numCurves):
            data = cmds.xgmGuideGeom(guide=self.guides[i], controlPoints=True)
            pointslist += data

        samp.setPositions(floatList2V3fArray(pointslist))
        self.schema.set(samp)


# %%

class SaveXGenDesWindow(QtWidgets.QDialog):
    class MultiSelectCheckBox(QtWidgets.QCheckBox):
        def __init__(self, column_name, parent=None):
            super().__init__(parent)
            self.column_name = column_name
            self.clicked.connect(self.on_clicked)

        def on_clicked(self, checked):
            window = self.find_window()
            if not window or not hasattr(window, 'table'):
                return
            table = window.table
            contents = window.contentList
            selected_rows = self.get_rows_to_changing(table)
            for row in selected_rows:
                if 0 <= row < len(contents):
                    content = contents[row]
                    checkbox = getattr(content, self.column_name)
                    if checkbox is not None:
                        checkbox.blockSignals(True)
                        checkbox.setChecked(checked)
                        checkbox.blockSignals(False)

        def find_window(self):
            parent = self.parent()
            while parent:
                if (isinstance(parent, SaveXGenDesWindow) or
                        parent.__class__.__name__ == 'SaveXGenDesWindow'):
                    return parent
                parent = parent.parent()
            return None

        def get_rows_to_changing(self, table):
            pos = self.mapTo(table.viewport(), QtCore.QPoint(0, 0))
            _index = table.indexAt(pos).row()
            selected_rows = [index.row() for index in table.selectionModel().selectedRows()]
            return selected_rows if _index in selected_rows else [_index]

    class Content:
        def __init__(self, fnDepNode, showName, groupName, useGuide, bakeUV, animation, export):
            self.showName = showName
            self.fnDepNode = fnDepNode
            self.groupName = QtWidgets.QLineEdit()
            self.groupName.setText(groupName)
            self.useGuide = SaveXGenDesWindow.MultiSelectCheckBox("useGuide")
            self.useGuide.setChecked(useGuide)
            self.bakeUV = SaveXGenDesWindow.MultiSelectCheckBox("bakeUV")
            self.bakeUV.setChecked(bakeUV)
            self.animation = SaveXGenDesWindow.MultiSelectCheckBox("animation")
            self.animation.setChecked(animation)
            self.export = SaveXGenDesWindow.MultiSelectCheckBox("export")
            self.export.setChecked(export)
            self.splineAnimation = False
            self.writePtexGuideId = False
            self.regionPtex = ""

    def __init__(self, parent=mayaWindow()):
        super(SaveXGenDesWindow, self).__init__(parent)
        self.contentList: List[SaveXGenDesWindow.Content] = []
        self.save_path = '.'
        self.bakeMesh = None
        self.setWindowTitle("Export XGen description to UE Groom v{}".format(_XGenExporterVersion))
        self.setGeometry(400, 400, 1130, 550)
        self.buildUI()

    def showAbout(self):
        QtWidgets.QMessageBox.about(self, "Export XGen to UE Groom",
                                    "A small tool to export XGen to UE Groom, by PDE26jjk. Link:  <a href='https://github.com/PDE26jjk/XGenUEGroomExporter'>https://github.com/PDE26jjk/XGenUEGroomExporter</a>")

    def createFrame(self, labelText):
        try:
            frame = om1ui.MQtUtil.findControl(
                cmds.frameLayout(label=labelText, collapsable=True, collapse=True, manage=True))
            frame: QtWidgets.QWidget = shiboken.wrapInstance(int(frame), QtWidgets.QWidget)
            frame.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Maximum)
            frameLayout: QtWidgets.QLayout = frame.children()[2].children()[0]
        except:
            frame = QtWidgets.QFrame(self)
            frame.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Maximum)
            frameLayout = QtWidgets.QVBoxLayout(frame)
            frame.children().append(frameLayout)
        return frame, frameLayout

    def buildUI(self):
        main_layout = QtWidgets.QVBoxLayout()

        menu_bar = QtWidgets.QMenuBar(self)
        menu_bar.addMenu("Help").addAction("About", self.showAbout)
        main_layout.setMenuBar(menu_bar)

        label1 = QtWidgets.QLabel("Select XGen Description")
        label1.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Maximum)
        hBox = QtWidgets.QHBoxLayout()
        hBox.setContentsMargins(10, 4, 10, 4)
        hBox.addWidget(label1)

        self.fillWithSelectList_button = QtWidgets.QPushButton("Refresh selected")
        self.fillWithSelectList_button.clicked.connect(self.fillTableWithSelectList)
        self.fillWithSelectList_button.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Maximum)
        # main_layout.addWidget(self.fillWithSelectList_button)
        hBox.addStretch(1)
        hBox.addWidget(self.fillWithSelectList_button)
        main_layout.addLayout(hBox)

        self.table = QtWidgets.QTableWidget(self)
        self.table.setColumnCount(7)
        self.table.setHorizontalHeaderLabels(["", "Name", "Group name", "Use guide", "Bake UV", "Animation", ""])
        self.table.horizontalHeader().setSectionResizeMode(0, QtWidgets.QHeaderView.Fixed)
        self.table.setColumnWidth(0, 40)
        self.table.setColumnWidth(3, 140)
        self.table.horizontalHeader().setSectionResizeMode(3, QtWidgets.QHeaderView.Fixed)
        self.table.setColumnWidth(4, 140)
        self.table.horizontalHeader().setSectionResizeMode(4, QtWidgets.QHeaderView.Fixed)
        self.table.setColumnWidth(5, 140)
        self.table.horizontalHeader().setSectionResizeMode(5, QtWidgets.QHeaderView.Fixed)
        self.table.setColumnWidth(6, 140)
        self.table.horizontalHeader().setSectionResizeMode(6, QtWidgets.QHeaderView.Fixed)
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.setSelectionMode(QtWidgets.QAbstractItemView.ExtendedSelection)  # Multi Selection
        self.table.setSelectionBehavior(QtWidgets.QTableView.SelectRows)

        self.table.setStyleSheet("""
            QTableView::item
            {
              border: 0px;
              padding: 5px;
              background-color: rgb(68, 68, 68); 
            }
            QTableView::item:selected {
              background-color: rgb(81, 133, 166); 
            }
            QTableView::item QCheckBox {  
                padding-left:60px;
            }
        """)

        self.table.clearContents()
        self.table.setRowCount(0)

        self.splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)

        # self.table.cellClicked.connect(self.update_detail)
        self.table.selectionModel().selectionChanged.connect(self.update_detail)
        self.splitter.addWidget(self.table)

        # Detail view on the right
        self.detail_widget = None
        self.clear_detail()
        # self.splitter.setSizes([1,0])

        self.Bakeframe, frameLayout = self.createFrame(labelText="Bake UV")

        self.MeshName = QtWidgets.QLabel("Mesh : ---")
        hBox = QtWidgets.QHBoxLayout()
        hBox.setContentsMargins(10, 10, 10, 10)
        hBox.addWidget(self.MeshName)
        hBox2 = QtWidgets.QHBoxLayout()
        label = QtWidgets.QLabel("UV Set : ")
        hBox2.addWidget(label)
        self.combo = QtWidgets.QComboBox()
        self.combo.addItem("     ---     ")

        self.uvSetStr = QtWidgets.QLabel("Selected: None")

        self.combo.currentIndexChanged.connect(self.update_uvset_label)
        hBox2.addWidget(self.combo)
        hBox.addStretch(2)
        hBox.addLayout(hBox2)
        hBox.addStretch(1)

        frameLayout.addLayout(hBox)

        self.button3 = QtWidgets.QPushButton("Pick other mesh", self)
        self.button3.clicked.connect(self.pick_mesh)
        self.button3.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Maximum)
        frameLayout.addWidget(self.button3)

        self.separator = QtWidgets.QFrame(self)
        self.separator.setFrameShape(QtWidgets.QFrame.HLine)
        self.separator.setFrameShadow(QtWidgets.QFrame.Sunken)

        self.AnimationFrame, frameLayout = self.createFrame(labelText="Animation")

        validator = QtGui.QIntValidator()
        validator.setRange(0, 99999)
        self.startFrame = QtWidgets.QLineEdit()
        self.startFrame.setMaximumWidth(60)
        self.startFrame.setValidator(validator)
        self.startFrame.setText(str(0))
        self.endFrame = QtWidgets.QLineEdit()
        self.endFrame.setMaximumWidth(60)
        self.endFrame.setValidator(validator)
        self.endFrame.setText(str(0))
        self.preroll = QtWidgets.QCheckBox("Preroll")

        frameLayout.setContentsMargins(10, 10, 10, 10)
        hBox = QtWidgets.QHBoxLayout()
        hBox.addWidget(QtWidgets.QLabel("Frame Range : "))
        hBox.addWidget(self.startFrame)
        hBox.addWidget(QtWidgets.QLabel(" ~ "))
        hBox.addWidget(self.endFrame)
        hBox.addStretch(1)
        hBox2 = QtWidgets.QHBoxLayout()
        hBox2.addWidget(self.preroll)
        hBox2.addStretch(1)

        frameLayout.addLayout(hBox)
        frameLayout.addLayout(hBox2)

        self.SettingFrame, frameLayout = self.createFrame(labelText="Setting")

        frameLayout.setContentsMargins(10, 10, 10, 10)
        hBox = QtWidgets.QHBoxLayout()
        self.createGroupId_cb = QtWidgets.QCheckBox("Create group id")
        self.createGroupId_cb.setChecked(True)
        hBox.addWidget(self.createGroupId_cb)

        self.createCardId_cb = QtWidgets.QCheckBox("Create card id same as group name")
        self.createCardId_cb.setChecked(False)
        hBox.addWidget(self.createCardId_cb)

        frameLayout.addLayout(hBox)

        self.save_button = QtWidgets.QPushButton("Save Alembic File", self)
        self.save_button.clicked.connect(self.save_abc)
        self.clear_temp_button = QtWidgets.QPushButton("Clear Temp Data", self)
        self.clear_temp_button.clicked.connect(self.clear_temp)
        self.cancel_button = QtWidgets.QPushButton("Close", self)
        self.cancel_button.clicked.connect(self.close)

        button_layout = QtWidgets.QHBoxLayout()
        button_layout.addWidget(self.save_button)
        button_layout.addWidget(self.clear_temp_button)
        button_layout.addWidget(self.cancel_button)

        main_layout.addWidget(self.splitter)
        main_layout.addWidget(self.Bakeframe)
        main_layout.addWidget(self.AnimationFrame)
        main_layout.addWidget(self.SettingFrame)
        main_layout.addWidget(self.separator)
        main_layout.addLayout(button_layout)

        self.setLayout(main_layout)

    def clear_detail(self):
        old_sizes = None
        if self.detail_widget is not None:
            old_sizes = self.splitter.sizes()
            self.detail_widget.setParent(None)
        self.detail_label = QtWidgets.QLabel("Select an item to view details")
        self.detail_label.setAlignment(QtCore.Qt.AlignTop | QtCore.Qt.AlignLeft)
        self.detail_widget = QtWidgets.QWidget()
        self.detail_layout = QtWidgets.QVBoxLayout()
        self.detail_layout.addWidget(self.detail_label)
        self.detail_widget.setLayout(self.detail_layout)
        self.splitter.addWidget(self.detail_widget)
        # self.detail_widget.setMaximumWidth(1000)
        if old_sizes is None:
            self.splitter.setStretchFactor(0, 4)
            self.splitter.setStretchFactor(1, 1)
        else:
            self.splitter.setSizes(old_sizes)

    def create_detail_checkBox(self, prop):
        selected_rows = [index.row() for index in self.table.selectionModel().selectedRows()]
        if len(selected_rows) == 0:
            return None
        checkBox = QtWidgets.QCheckBox(self)
        isTrue = getattr(self.contentList[selected_rows[0]], prop)
        checkBox.setChecked(isTrue)
        for row in selected_rows:
            content = self.contentList[row]
            if getattr(content, prop) != isTrue:
                checkBox.setCheckState(QtCore.Qt.CheckState.PartiallyChecked)
                break

        def onStateChange(state):
            for row in selected_rows:
                content = self.contentList[row]
                setattr(content, prop, state == 2)

        checkBox.stateChanged.connect(onStateChange)
        return checkBox

    def update_detail(self, indices):
        self.clear_detail()
        selected_rows = [index.row() for index in self.table.selectionModel().selectedRows()]
        if len(selected_rows) == 0:
            return
        content = self.contentList[selected_rows[0]]
        is_multi_selected = len(self.table.selectionModel().selectedRows()) > 1
        vBox = QtWidgets.QVBoxLayout()
        self.detail_layout.addLayout(vBox)
        vBox2 = QtWidgets.QVBoxLayout()
        self.detail_layout.addLayout(vBox2)
        vBox2.addStretch(1)

        self.detail_label.setText(content.showName if not is_multi_selected else "--")
        self.detail_label.setStyleSheet('font-weight:bold;margin-bottom:20px')
        self.detail_label.setAlignment(QtCore.Qt.AlignTop | QtCore.Qt.AlignCenter)  # 设置对齐方式

        write_spline_animation = self.create_detail_checkBox('splineAnimation')
        write_spline_animation.setText("write spline animation")
        write_spline_animation.setToolTip(
            "If writes animation, also write spline animation, not just the animation of guides.")

        vBox.addWidget(write_spline_animation)

        write_guide_id_cb = self.create_detail_checkBox('writePtexGuideId')
        write_guide_id_cb.setText("write guide id from ptex")
        write_guide_id_cb.setToolTip(
            "Experimental feature, only supports versions up to UE5.3, writes properties such as groom_closest_guides.")

        # vBox.setContentsMargins(0,0,10,10)
        vBox.addWidget(write_guide_id_cb)

        if is_multi_selected:
            group_name_multi_edit = QtWidgets.QLineEdit()

            names_are_same = True
            group_name = content.groupName.text()
            for selected_row in selected_rows[1:]:
                if self.contentList[selected_row].groupName.text() != group_name:
                    names_are_same = False
                    break
            if names_are_same:
                group_name_multi_edit.setText(group_name)
            else:
                group_name_multi_edit.setPlaceholderText(" -- ")

            def change_group_name(text):
                for selected_row in selected_rows:
                    self.contentList[selected_row].groupName.setText(text)

            group_name_multi_edit.textEdited.connect(change_group_name)
            hBox = QtWidgets.QHBoxLayout()
            group_name_label = QtWidgets.QLabel("Group name: ")
            hBox.addWidget(group_name_label)
            hBox.addWidget(group_name_multi_edit)
            vBox.addLayout(hBox)

        if not is_multi_selected:
            label = QtWidgets.QLabel('Select .ptx file:')
            # label.setStyleSheet('font-size:16px')
            vBox.addWidget(label)

            def setPath(text):
                content.regionPtex = text

            RegionPtex = FileSelectorWidget(setPath)
            RegionPtex.set_file_path(content.regionPtex)
            vBox.addWidget(RegionPtex)

            self.splitter.addWidget(self.detail_widget)

    def pick_mesh(self):
        selectionList = om.MGlobal.getActiveSelectionList()
        if selectionList.length() > 0:
            dag_path = selectionList.getDagPath(0)
            fnDepNode = om.MFnDependencyNode(dag_path.node())
            itDag = om.MItDag()
            # find mesh
            itDag.reset(fnDepNode.object(), om.MItDag.kDepthFirst, om.MFn.kMesh)
            while not itDag.isDone():
                meshPath = om.MDagPath.getAPathTo(itDag.currentItem())
                mesh = om.MFnMesh(meshPath)
                self.setBakeMesh(mesh)
                break

    def update_uvset_label(self):
        selected_option = self.combo.currentText()
        self.uvSetStr.setText(selected_option)

    def clear_temp(self):
        deleteSaveXGenDesWindowParent()

    def save_abc(self):
        if len(self.contentList) == 0:
            print("No content")
            return
        file_path = cmds.fileDialog2(
            caption="Save Alembic File",
            fileMode=0,
            okCaption="save",
            startingDirectory=self.save_path,
            ff='Alembic Files (*.abc);;All Files (*)'
        )
        if file_path:
            self.save_path = file_path[0]
        else:
            return
        selectionList = om.MGlobal.getActiveSelectionList()
        startTime = time.time()
        oldCurTime = omAnim.MAnimControl.currentTime()
        archive = abc.OArchive(file_path[0])

        anyAnimation = False
        for item in self.contentList:
            if item.export.isChecked():
                hasAnimation = item.animation.isChecked()
                if hasAnimation:
                    anyAnimation = True

        if anyAnimation:
            frameRange = [int(self.startFrame.text()), int(self.endFrame.text())]
            if (frameRange[0] > frameRange[1]
                    or frameRange[0] < omAnim.MAnimControl.minTime().value
                    or frameRange[1] > omAnim.MAnimControl.maxTime().value):
                raise ValueError("Frame out of range.")
            # frameRange[0] = int(max(frameRange[0], omAnim.MAnimControl.minTime().value))
            # frameRange[1] = int(min(frameRange[1], omAnim.MAnimControl.maxTime().value))

            sec = om.MTime(1, om.MTime.kSeconds)
            spf = 1.0 / sec.asUnits(om.MTime.uiUnit())
            timeSampling = abcA.TimeSampling(spf, spf * frameRange[0])

            timeIndex = archive.addTimeSampling(timeSampling)
        proxyList = []  # All Alembic content should be destroyed at the end of the method, otherwise it will not be written to the file
        setGroomGuideIdStartIndex(0)
        for item in self.contentList:
            if item.export.isChecked():
                fnDepNode = item.fnDepNode
                needBakeUV = item.bakeUV.isChecked()
                hasAnimation = item.animation.isChecked()
                useGuide = item.useGuide.isChecked()
                if hasAnimation:
                    curveObj = abcGeom.OCurves(archive.getTop(), fnDepNode.name(), timeIndex)
                else:
                    curveObj = abcGeom.OCurves(archive.getTop(), fnDepNode.name())
                xgenProxy = XGenProxyEveryFrame(curveObj, item.fnDepNode, needBakeUV | useGuide, item.splineAnimation)
                xgenProxy.needBakeUV = needBakeUV
                xgenProxy.write_group_name(item.groupName.text(), self.createCardId_cb.isChecked())
                if useGuide:
                    # guides = GuidesToCurves(item.fnDepNode)
                    guideName = fnDepNode.name() + "_guide"
                    if hasAnimation:
                        curveObj = abcGeom.OCurves(archive.getTop(), guideName, timeIndex)
                    else:
                        curveObj = abcGeom.OCurves(archive.getTop(), guideName)
                    guideProxy = GuideProxy(curveObj, fnDepNode, False, hasAnimation)
                    guideProxy.write_group_name(item.groupName.text(), self.createCardId_cb.isChecked())
                    guideProxy.write_is_guide(True)
                    proxyList.append(guideProxy)
                    guideProxy.set_xgen_proxy_and_ptex(xgenProxy, item.regionPtex)
                    guideProxy.writePtexGuideId = item.writePtexGuideId
                proxyList.append(xgenProxy)  # after guides, for baking
        # return
        if len(proxyList) == 0:
            print("No content")
            om.MGlobal.setActiveSelectionList(selectionList)
            return

        if self.createGroupId_cb.isChecked():
            groupIds = dict()
            currentId = 0
            for proxy in proxyList:
                if proxy.groupName not in groupIds:
                    groupIds[proxy.groupName] = currentId
                    currentId += 1
                proxy.write_group_id(groupIds[proxy.groupName])

        if anyAnimation:
            if self.preroll.isChecked():
                for frame in range(int(omAnim.MAnimControl.minTime().value), frameRange[0]):
                    om.MGlobal.viewFrame(frame)
            for frame in range(frameRange[0], frameRange[1] + 1):
                om.MGlobal.viewFrame(frame)
                for item in proxyList:
                    if frame == frameRange[0]:
                        item.write_first_frame()
                    elif item.animation:
                        item.write_frame()
            omAnim.MAnimControl.setCurrentTime(oldCurTime)
        else:
            for item in proxyList:
                item.write_first_frame()
        for item in proxyList:
            item.bake_uv(self.bakeMesh, self.uvSetStr.text())
            if isinstance(item, GuideProxy):
                item.write_guide_id_from_ptex()
        print("Data has been saved in %s, it took %.2f seconds." % (file_path[0], time.time() - startTime))
        om.MGlobal.setActiveSelectionList(selectionList)
        return file_path[0]

    def fillTableWithSelectList(self):
        self.clear_detail()
        self.contentList = []
        selectionList = om.MGlobal.getActiveSelectionList()
        contentList = []
        for i in range(selectionList.length()):
            dag_path = selectionList.getDagPath(i)
            fnDepNode = om.MFnDependencyNode(dag_path.node())
            if fnDepNode.typeName == 'xgmPalette':
                continue
            itDag = om.MItDag()
            # find xgen description
            itDag.reset(fnDepNode.object(), om.MItDag.kDepthFirst, om.MFn.kNamedObject)
            xgDes = None
            while not itDag.isDone():
                dn = om.MFnDependencyNode(itDag.currentItem())
                if dn.typeName == 'xgmDescription':
                    xgDes = dn
                    break
                itDag.next()
            if xgDes is not None:
                content = SaveXGenDesWindow.Content(xgDes, fnDepNode.name(), fnDepNode.name(), True,
                                                    False, False, True)
                content.regionPtex = getClumpingPtexPath(fnDepNode)
                contentList.append(content)
                boundMesh = self.findBoundMesh(xgDes)
                if boundMesh is not None:
                    self.setBakeMesh(boundMesh)

        self.table.setRowCount(len(contentList))
        for row in range(len(contentList)):
            self.table.setCellWidget(row, 0, contentList[row].export)
            contentList[row].export.setStyleSheet("padding-left:8px")
            item = QtWidgets.QTableWidgetItem(contentList[row].showName)
            item.setFlags(QtCore.Qt.ItemFlag.ItemIsEnabled | QtCore.Qt.ItemFlag.ItemIsSelectable)
            self.table.setItem(row, 1, item)

            self.table.setCellWidget(row, 2, contentList[row].groupName)
            self.table.setCellWidget(row, 3, contentList[row].useGuide)
            self.table.setCellWidget(row, 4, contentList[row].bakeUV)
            self.table.setCellWidget(row, 5, contentList[row].animation)

        self.contentList = contentList

    def findBoundMesh(self, xgDes):
        itDg = om.MItDag()
        itDg.reset(om.MFnDagNode(xgDes.object()).parent(0), om.MItDag.kDepthFirst, om.MFn.kPluginShape)
        boundMesh = None
        while not itDg.isDone():
            dn = om.MFnDependencyNode(itDg.currentItem())
            if dn.typeName == 'xgmSubdPatch':
                boundMeshPlug: om.MPlug = dn.findPlug('geometry', False)
                boundMesh = om.MFnMesh(
                    om.MDagPath.getAPathTo(boundMeshPlug.source().node()))
                break
            itDg.next()
        return boundMesh

    def setBakeMesh(self, mesh: om.MFnMesh):
        if mesh is not None:
            self.bakeMesh = mesh
            self.MeshName.setText(f"Mesh: {mesh.name()}")
            self.combo.clear()
            self.combo.addItems(mesh.getUVSetNames())


SaveXGenDesWindowInstanceName = '_SaveXGenDesWindowInstance'
if SaveXGenDesWindowInstanceName not in globals():
    globals()[SaveXGenDesWindowInstanceName] = SaveXGenDesWindow()
globals()[SaveXGenDesWindowInstanceName].show()
# SaveXGenDesWindow().show()
