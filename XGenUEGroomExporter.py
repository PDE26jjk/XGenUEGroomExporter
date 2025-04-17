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

# %%
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
def getXgenData(fnDepNode: om.MFnDependencyNode):
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

    PrimitiveInfosList = []
    PositionsDataList = []
    WidthsDataList = []
    for k, v in Items.items():
        # print(k, len(v))
        if k == 'PrimitiveInfos':
            dtype_format = '<IQ'
            for addr in v:
                decompressed_data = decompressData(*addr)  # 假设解压缩返回字节流
                PrimitiveInfos = []
                record_size = struct.calcsize(dtype_format)
                for i in range(0, len(decompressed_data), record_size):
                    PrimitiveInfo = struct.unpack_from(f'{dtype_format}', decompressed_data, i)
                    PrimitiveInfos.append(PrimitiveInfo)

                PrimitiveInfosList.append(PrimitiveInfos)

        if k == 'Positions':
            for addr in v:
                decompressed_data = decompressData(*addr)
                posData = array.array('f', decompressed_data)

                PositionsDataList.append(posData)

        if k == 'WIDTH_CV':
            for addr in v:
                decompressed_data = decompressData(*addr)
                widthData = array.array('f', decompressed_data)
                WidthsDataList.append(widthData)

    return PrimitiveInfosList, PositionsDataList, WidthsDataList


# %%
class CurvesProxy:
    def __init__(self, curveObj: abcGeom.OCurves, fnDepNode: om.MFnDependencyNode, needBakeUV=False, animation=False):
        self.hairRootList = None
        self.schema: abcGeom.OCurvesSchema = curveObj.getSchema()
        self.needBakeUV = needBakeUV
        self.animation = animation
        self.firstSamp = abcGeom.OCurvesSchemaSample()
        self.fnDepNode = fnDepNode
        self.curves = None
        self.groupName = None

    def write_group_name(self, group_name: str):
        cp: abc.OCompoundProperty = self.schema.getArbGeomParams()
        groupName = abc.OStringArrayProperty(cp, "groom_group_name")
        groupName.setValue(list2ImathArray([group_name], imath.StringArray))
        self.groupName = group_name

    def write_is_guide(self, is_guide=True):
        cp: abc.OCompoundProperty = self.schema.getArbGeomParams()
        if is_guide:
            guideFlag = abc.OInt16ArrayProperty(cp, "groom_guide")
            guideFlag.setValue(list2ImathArray([1], imath.ShortArray))

    def write_group_id(self, group_id: int):
        cp: abc.OCompoundProperty = self.schema.getArbGeomParams()
        _id = abc.OInt32ArrayProperty(cp, "groom_group_id")
        _id.setValue(list2ImathArray([group_id], imath.IntArray))

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
        if self.needBakeUV:
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
            if self.needBakeUV:
                self.hairRootList.append(cvArray[0])
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
        if self.hairRootList is None:
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

        cp: abc.OCompoundProperty = self.schema.getArbGeomParams()
        uv_prop = abc.OV2fArrayProperty(cp, "groom_root_uv")
        uv_prop.setValue(uvs)


class XGenProxy(CurvesProxy):
    def __init__(self, curveObj: abcGeom.OCurves, fnDepNode: om.MFnDependencyNode, needBakeUV=False, animation=False):
        super().__init__(curveObj, fnDepNode, needBakeUV, animation)

    def write_first_frame(self):
        if print_debug:
            startTime = time.time()
        PrimitiveInfosList, PositionsDataList, WidthsDataList = getXgenData(self.fnDepNode)
        if print_debug:
            print("getXgenData: %.4f" % (time.time() - startTime))
            startTime = time.time()
        numCurves = 0
        numCVs = 0
        for i, PrimitiveInfos in enumerate(PrimitiveInfosList):
            numCurves += len(PrimitiveInfos)
            for PrimitiveInfo in PrimitiveInfos:
                numCVs += PrimitiveInfo[1]

        orders = imath.UnsignedCharArray(numCurves)
        nVertices = imath.IntArray(numCurves)
        cp: abc.OCompoundProperty = self.schema.getArbGeomParams()

        samp = self.firstSamp
        samp.setBasis(abcGeom.BasisType.kBsplineBasis)
        samp.setWrap(abcGeom.CurvePeriodicity.kNonPeriodic)
        samp.setType(abcGeom.CurveType.kCubic)

        degree = 3
        pointArray = imath.V3fArray(numCVs)
        widthArray = imath.FloatArray(numCVs)
        if self.needBakeUV:
            self.hairRootList = []
        knots = []

        curveIndex = 0
        cvIndex = 0

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
                for k in range(length):
                    pointArray[cvIndex].x = posData[startAddr]
                    pointArray[cvIndex].y = posData[startAddr + 1]
                    pointArray[cvIndex].z = posData[startAddr + 2]
                    if k == 0 and self.needBakeUV:
                        self.hairRootList.append(om.MPoint(pointArray[cvIndex]))
                    widthArray[cvIndex] = widthData[offset + k]
                    startAddr += 3
                    cvIndex += 1

                orders[curveIndex] = degree + 1
                nVertices[curveIndex] = length

                knotsInsideNum = length - degree + 1
                knotsList = [*([0] * degree), *list(range(knotsInsideNum)),
                             *([knotsInsideNum - 1] * degree )]
                knots += knotsList
                curveIndex += 1

        samp.setCurvesNumVertices(nVertices)
        samp.setPositions(pointArray)
        samp.setKnots(list2ImathArray(knots, imath.FloatArray))
        samp.setOrders(orders)

        # bake vertex color example
        # cvColor = abcGeom.OC3fGeomParam(cp, "groom_color", False, abcGeom.GeometryScope.kVertexScope, 1)
        # cvColorArray = imath.C3fArray(len(pointslist) // 3)
        # i = 0
        # color1 = imath.Color3f((0, 1, 1));
        # color2 = imath.Color3f((1, 0, 1))
        # for _ in range(len(nVertices)):
        #     length = nVertices[_]
        #     for j in range(length):
        #         t = (j / (length - 1))
        #         cvColorArray[i] = color1 * (1 - t) + color2 * t
        #         i += 1
        # cvColorArray = abcGeom.OC3fGeomParamSample(cvColorArray, abcGeom.GeometryScope.kVertexScope)
        # cvColor.set(cvColorArray)

        # write width
        widths = abcGeom.OFloatGeomParamSample(widthArray, abcGeom.GeometryScope.kVertexScope)
        samp.setWidths(widths)
        self.schema.set(samp)

        if print_debug:
            print("write_first_frame: %.4f" % (time.time() - startTime))

    def write_frame(self):
        if print_debug:
            startTime = time.time()
        PrimitiveInfosList, PositionsDataList, WidthsDataList = getXgenData(self.fnDepNode)
        numCurves = 0
        numCVs = 0
        for i, PrimitiveInfos in enumerate(PrimitiveInfosList):
            numCurves += len(PrimitiveInfos)
            for PrimitiveInfo in PrimitiveInfos:
                numCVs += PrimitiveInfo[1]

        cp: abc.OCompoundProperty = self.schema.getArbGeomParams()

        samp = abcGeom.OCurvesSchemaSample()
        samp.setBasis(self.firstSamp.getBasis())
        samp.setWrap(self.firstSamp.getWrap())
        samp.setType(self.firstSamp.getType())

        samp.setCurvesNumVertices(self.firstSamp.getCurvesNumVertices())
        samp.setKnots(self.firstSamp.getKnots())
        samp.setOrders(self.firstSamp.getOrders())
        samp.setWidths(self.firstSamp.getWidths())

        pointArray = imath.V3fArray(numCVs)

        curveIndex = 0
        cvIndex = 0
        for j in range(len(PrimitiveInfosList)):
            PrimitiveInfos = PrimitiveInfosList[j]
            posData = PositionsDataList[j]
            for i, PrimitiveInfo in enumerate(PrimitiveInfos):
                offset = PrimitiveInfo[0]
                length = int(PrimitiveInfo[1])
                if length < 2:
                    continue
                startAddr = offset * 3
                for k in range(length):
                    pointArray[cvIndex].x = posData[startAddr]
                    pointArray[cvIndex].y = posData[startAddr + 1]
                    pointArray[cvIndex].z = posData[startAddr + 2]
                    startAddr += 3
                    cvIndex += 1

                curveIndex += 1

        samp.setPositions(pointArray)

        self.schema.set(samp)
        if print_debug:
            print("write_frame: %.4f" % (time.time() - startTime))


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


# %%
class SaveXGenWindow(QtWidgets.QDialog):
    class Content:
        def __init__(self, fnDepNode, showName, Type, groupName, isGuide, bakeUV, animation, export):
            self.showName = showName
            self.fnDepNode = fnDepNode
            self.Type = Type
            self.groupName = QtWidgets.QLineEdit()
            self.groupName.setText(groupName)
            self.isGuide = QtWidgets.QCheckBox()
            self.isGuide.setChecked(isGuide)
            self.bakeUV = QtWidgets.QCheckBox()
            self.bakeUV.setChecked(bakeUV)
            self.animation = QtWidgets.QCheckBox()
            self.animation.setChecked(animation)
            self.export = QtWidgets.QCheckBox()
            self.export.setChecked(export)

    curveType = "curve"
    xgenType = "xgen"

    def __init__(self, parent=mayaWindow()):
        super(SaveXGenWindow, self).__init__(parent)
        self.contentList: List[SaveXGenWindow.Content] = []
        self.save_path = '.'
        self.bakeMesh = None
        self.setWindowTitle("Export XGen to UE Groom")
        self.setGeometry(400, 400, 850, 450)
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

        label1 = QtWidgets.QLabel("Please select Curves and Interactive XGen")  # 请选择曲线和交互式XGen
        label1.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Maximum)
        hBox = QtWidgets.QHBoxLayout()
        hBox.setContentsMargins(10, 4, 10, 4)
        hBox.addWidget(label1)

        self.fillWithSelectList_button = QtWidgets.QPushButton("Refresh selected")  # 刷新选择
        self.fillWithSelectList_button.clicked.connect(self.fillWithSelectList)
        self.fillWithSelectList_button.setSizePolicy(QtWidgets.QSizePolicy.Maximum, QtWidgets.QSizePolicy.Maximum)
        # main_layout.addWidget(self.fillWithSelectList_button)
        hBox.addStretch(1)
        hBox.addWidget(self.fillWithSelectList_button)
        main_layout.addLayout(hBox)

        self.table = QtWidgets.QTableWidget(self)
        self.table.setColumnCount(8)  # 设置列数
        self.table.setHorizontalHeaderLabels(["", "Name", "Type", "Group name", "Is guide", "Bake UV", "Animation", ""])
        self.table.horizontalHeader().setSectionResizeMode(0, QtWidgets.QHeaderView.Fixed)
        self.table.setColumnWidth(0, 40)
        self.table.horizontalHeader().setSectionResizeMode(2, QtWidgets.QHeaderView.ResizeToContents)
        self.table.setColumnWidth(4, 140)
        self.table.horizontalHeader().setSectionResizeMode(4, QtWidgets.QHeaderView.Fixed)
        self.table.setColumnWidth(5, 140)
        self.table.horizontalHeader().setSectionResizeMode(5, QtWidgets.QHeaderView.Fixed)
        self.table.setColumnWidth(6, 140)
        self.table.horizontalHeader().setSectionResizeMode(6, QtWidgets.QHeaderView.Fixed)
        self.table.horizontalHeader().setStretchLastSection(True)

        self.table.setStyleSheet("""
            QTableView::item
            {
              border: 0px;
              padding: 5px;
              background-color: rgb(68, 68, 68); 
            }
            QTableView::item QCheckBox {  
                padding-left:60px;
            }
        """)

        self.table.clearContents()
        self.table.setRowCount(0)

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

        self.combo.currentIndexChanged.connect(self.update_label)
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

        frameLayout.addLayout(hBox)

        self.save_button = QtWidgets.QPushButton("Save Alembic File", self)
        self.save_button.clicked.connect(self.save_abc)

        self.cancel_button = QtWidgets.QPushButton("Close", self)  # 关闭
        self.cancel_button.clicked.connect(self.close)  # 关闭窗口

        button_layout = QtWidgets.QHBoxLayout()
        button_layout.addWidget(self.save_button)
        button_layout.addWidget(self.cancel_button)

        main_layout.addWidget(self.table)
        main_layout.addWidget(self.Bakeframe)
        main_layout.addWidget(self.AnimationFrame)
        main_layout.addWidget(self.SettingFrame)
        main_layout.addWidget(self.separator)
        main_layout.addLayout(button_layout)

        self.setLayout(main_layout)

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

    def update_label(self):
        selected_option = self.combo.currentText()
        self.uvSetStr.setText(selected_option)

    def save_abc(self):
        if len(self.contentList) == 0:
            print("No content")
            return
        file_path = cmds.fileDialog2(
            # dialogStyle=2,
            caption="Save as Alembic File",
            fileMode=0,
            okCaption="save",
            # defaultExtension='abc',
            startingDirectory=self.save_path,
            ff='Alembic Files (*.abc);;All Files (*)'
        )
        if file_path:
            self.save_path = file_path[0]
        else:
            return
        startTime = time.time()
        oldCurTime = omAnim.MAnimControl.currentTime()
        archive = abc.OArchive(file_path[0])

        anyAnimation = False
        for item in self.contentList:
            hasAnimation = item.animation.isChecked()
            if hasAnimation:
                anyAnimation = True
                break
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

        proxyList: List[CurvesProxy] = []
        for item in self.contentList:
            if not item.export:
                continue
            fnDepNode = item.fnDepNode
            needBakeUV = item.bakeUV.isChecked()
            hasAnimation = item.animation.isChecked()
            if hasAnimation:
                curveObj = abcGeom.OCurves(archive.getTop(), fnDepNode.name(), timeIndex)
            else:
                curveObj = abcGeom.OCurves(archive.getTop(), fnDepNode.name())

            if item.Type == SaveXGenWindow.xgenType:
                proxy = XGenProxy(curveObj, fnDepNode, needBakeUV, hasAnimation)
            elif item.Type == SaveXGenWindow.curveType:
                proxy = CurvesProxy(curveObj, fnDepNode, needBakeUV, hasAnimation)
            else:
                continue
            proxyList.append(proxy)
            proxy.write_group_name(item.groupName.text())
            proxy.write_is_guide(item.isGuide.isChecked())

        if len(proxyList) == 0:
            print("No content")
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
                    item.bake_uv(self.bakeMesh, self.uvSetStr.text())
            omAnim.MAnimControl.setCurrentTime(oldCurTime)
        else:
            for item in proxyList:
                item.write_first_frame()
                item.bake_uv(self.bakeMesh, self.uvSetStr.text())
        print("Data has been saved in %s, it took %.2f seconds." % (file_path[0], time.time() - startTime))

        return file_path[0]

    def fillWithSelectList(self):
        self.contentList = []
        selectionList = om.MGlobal.getActiveSelectionList()
        contentList = []
        for i in range(selectionList.length()):
            dag_path = selectionList.getDagPath(i)
            fnDepNode = om.MFnDependencyNode(dag_path.node())
            itDag = om.MItDag()
            # find xgen description
            itDag.reset(fnDepNode.object(), om.MItDag.kDepthFirst, om.MFn.kNamedObject)
            xgDes = None
            while not itDag.isDone():
                dn = om.MFnDependencyNode(itDag.currentItem())
                if dn.typeName == 'xgmSplineDescription':
                    xgDes = dn
                    break
                itDag.next()
            if xgDes is not None:
                contentList.append(
                    SaveXGenWindow.Content(xgDes, fnDepNode.name(), SaveXGenWindow.xgenType, fnDepNode.name(), False,
                                           False, False, True))
                boundMesh = self.findBoundMesh(xgDes)
                if boundMesh is not None:
                    self.setBakeMesh(boundMesh)
                continue
            # find curve
            itDag.reset(fnDepNode.object(), om.MItDag.kDepthFirst, om.MFn.kCurve)
            isCurve = False
            while not itDag.isDone():
                isCurve = True
                break
            if isCurve:
                groupNameStr: str = fnDepNode.name()
                suffix = "_guide"
                if groupNameStr.endswith(suffix):
                    groupNameStr = groupNameStr[:-len(suffix)]
                contentList.append(
                    SaveXGenWindow.Content(fnDepNode, fnDepNode.name(), SaveXGenWindow.curveType, groupNameStr, True,
                                           False, False, True))

        self.table.setRowCount(len(contentList))
        for row in range(len(contentList)):
            self.table.setCellWidget(row, 0, contentList[row].export)
            contentList[row].export.setStyleSheet("padding-left:8px")
            item = QtWidgets.QTableWidgetItem(contentList[row].showName)
            # item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
            item.setFlags(QtCore.Qt.ItemFlag.ItemIsEnabled)
            self.table.setItem(row, 1, item)

            item = QtWidgets.QTableWidgetItem(contentList[row].Type)
            # item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
            item.setFlags(QtCore.Qt.ItemFlag.ItemIsEnabled)
            self.table.setItem(row, 2, item)

            self.table.setCellWidget(row, 3, contentList[row].groupName)
            self.table.setCellWidget(row, 4, contentList[row].isGuide)
            self.table.setCellWidget(row, 5, contentList[row].bakeUV)
            self.table.setCellWidget(row, 6, contentList[row].animation)

        self.contentList = contentList

    def findBoundMesh(self, xgDes):
        itDg = om.MItDependencyGraph(xgDes.object(), direction=om.MItDependencyGraph.kUpstream)
        boundMesh = None
        while not itDg.isDone():
            dn = om.MFnDependencyNode(itDg.currentNode())
            if dn.typeName == 'xgmSplineBase':
                boundMeshPlug: om.MPlug = dn.findPlug('boundMesh', False)
                boundMesh = om.MFnMesh(
                    om.MDagPath.getAPathTo(boundMeshPlug.elementByLogicalIndex(0).source().node()))
                break
            itDg.next()
        return boundMesh

    def setBakeMesh(self, mesh: om.MFnMesh):
        if mesh is not None:
            self.bakeMesh = mesh
            self.MeshName.setText(f"Mesh: {mesh.name()}")
            self.combo.clear()
            self.combo.addItems(mesh.getUVSetNames())


SaveXGenWindowInstanceName = '_SaveXGenWindowInstance'
if SaveXGenWindowInstanceName not in globals():
    globals()[SaveXGenWindowInstanceName] = SaveXGenWindow()
globals()[SaveXGenWindowInstanceName].show()
