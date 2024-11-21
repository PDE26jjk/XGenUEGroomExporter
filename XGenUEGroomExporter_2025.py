# %%
import alembic.Abc as abc
import alembic.AbcGeom as abcGeom
import maya.OpenMaya as om1
import maya.api.OpenMaya as om
import imath
import numpy as np
import array
import ctypes
import zlib
import json
import maya.cmds as cmds
from typing import List
import time


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
        if k == 'PrimitiveInfos':
            dtype = np.dtype([('offset', 'u4'), ('length', 'u8')])
            for addr in v:
                PrimitiveInfos = np.frombuffer(decompressData(*addr), dtype=dtype)
                PrimitiveInfosList.append(PrimitiveInfos)
        if k == 'Positions':
            for addr in v:
                posData = array.array('f', decompressData(*addr))
                PositionsDataList.append(posData)
        if k == 'WIDTH_CV':
            for addr in v:
                widthData = array.array('f', decompressData(*addr))
                WidthsDataList.append(widthData)

    return PrimitiveInfosList, PositionsDataList, WidthsDataList


# %%
def write_group_and_guide(curveObj: abcGeom.OCurves, group_name: str, is_guide):
    curveschema: abcGeom.OCurvesSchema = curveObj.getSchema()
    cp: abc.OCompoundProperty = curveschema.getArbGeomParams()
    groupName = abc.OStringArrayProperty(cp, "groom_group_name")
    groupName.setValue(list2ImathArray([group_name], imath.StringArray))
    if is_guide:
        guideFlag = abc.OInt16ArrayProperty(cp, "groom_guide")
        guideFlag.setValue(list2ImathArray([1], imath.ShortArray))


def write_curves(curveObj: abcGeom.OCurves, fnDepNode: om.MFnDependencyNode, needHairRootList=False):
    itDag = om.MItDag()
    # help(itDag.reset)
    itDag.reset(fnDepNode.object(), om.MItDag.kDepthFirst, om.MFn.kCurve)
    curves = []
    while not itDag.isDone():
        curve_node = itDag.currentItem()
        curves.append(curve_node)
        itDag.next()
    if len(curves) == 0:
        return None
    curve = om.MFnNurbsCurve(curves[0])
    curveschema: abcGeom.OCurvesSchema = curveObj.getSchema()
    cp: abc.OCompoundProperty = curveschema.getArbGeomParams()

    numCurves = len(curves)

    orders = imath.IntArray(numCurves)
    nVertices = imath.IntArray(numCurves)
    pointslist = []
    knots = []
    hairRootlist = []
    samp = abcGeom.OCurvesSchemaSample()
    samp.setBasis(abcGeom.BasisType.kBsplineBasis)
    samp.setWrap(abcGeom.CurvePeriodicity.kNonPeriodic)

    if curve.degree == 3:
        samp.setType(abcGeom.CurveType.kCubic)
    elif curve.degree == 1:
        samp.setType(abcGeom.CurveType.kLinear)
    else:
        # samp.setType(abcGeom.CurveType.kVariableOrder) # https://github.com/alembic/alembic/issues/458 After alembic fixing this bug, use this line.
        samp.setType(abcGeom.CurveType.kLinear)
        # samp.setType(abcGeom.CurveType.kCubic)
        pass
    for i in range(numCurves):
        curve = curve.setObject(curves[i])
        numCVs = curve.numCVs
        orders[i] = curve.degree + 1
        nVertices[i] = numCVs
        cvArray = curve.cvPositions()
        for j in range(numCVs):
            pointslist.append(cvArray[j].x)
            pointslist.append(cvArray[j].y)
            pointslist.append(cvArray[j].z)
        if needHairRootList:
            hairRootlist.append(cvArray[0])
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
    curveschema.set(samp)
    if needHairRootList:
        return hairRootlist


def write_xgen(curveObj: abcGeom.OCurves, fnDepNode: om.MFnDependencyNode, needHairRootList=False):
    PrimitiveInfosList, PositionsDataList, WidthsDataList = getXgenData(fnDepNode)
    numCurves = 0
    numCVs = 0
    for i, PrimitiveInfos in enumerate(PrimitiveInfosList):
        numCurves += len(PrimitiveInfos)
        for PrimitiveInfo in PrimitiveInfos:
            numCVs += PrimitiveInfo[1]

    numCVs = int(numCVs)

    orders = imath.UnsignedCharArray(numCurves)
    nVertices = imath.IntArray(numCurves)
    curveschema: abcGeom.OCurvesSchema = curveObj.getSchema()
    cp: abc.OCompoundProperty = curveschema.getArbGeomParams()

    samp = abcGeom.OCurvesSchemaSample()
    samp.setBasis(abcGeom.BasisType.kBsplineBasis)
    samp.setWrap(abcGeom.CurvePeriodicity.kNonPeriodic)
    samp.setType(abcGeom.CurveType.kCubic)

    degree = 3
    # pointslist = []
    pointArray = imath.V3fArray(numCVs)
    widthArray = imath.FloatArray(numCVs)
    hairRootlist = []
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
            # pointslist += posData[offset:offset + length].reshape(-1).tolist()
            startAddr = offset * 3
            for k in range(length):
                pointArray[cvIndex].x = posData[startAddr]
                pointArray[cvIndex].y = posData[startAddr + 1]
                pointArray[cvIndex].z = posData[startAddr + 2]
                if k == 0 and needHairRootList:
                    hairRootlist.append(om.MPoint(pointArray[cvIndex]))
                widthArray[cvIndex] = widthData[offset + k]
                startAddr += 3
                cvIndex += 1
            orders[curveIndex] = degree + 1
            nVertices[curveIndex] = length

            degree = 3
            knotsInsideNum = length - degree + 1
            knotsList = [*([0] * (degree - 1)), *list(range(knotsInsideNum)), *([knotsInsideNum - 1] * (degree - 1))]
            knots += knotsList
            curveIndex += 1

    samp.setCurvesNumVertices(nVertices)
    # samp.setPositions(floatList2V3fArray(pointslist))
    samp.setPositions(pointArray)
    samp.setKnots(list2ImathArray(knots, imath.FloatArray))
    # samp.setOrders(list2ImathArray(orders, imath.UnsignedCharArray))
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
    curveschema.set(samp)
    if needHairRootList:
        return hairRootlist



def bake_uv(curveObj: abcGeom.OCurves, hairRootList: list, bakeMesh: om.MFnMesh, uv_set: str = None):
    if bakeMesh is None:
        return
    if uv_set is None:
        uv_set = bakeMesh.currentUVSetName()
    elif uv_set not in bakeMesh.getUVSetNames():
        raise Exception(f'Invalid UV Set : {uv_set}')

    uvs = imath.V2fArray(len(hairRootList))
    for i, hairRoot in enumerate(hairRootList):
        res = bakeMesh.getUVAtPoint(hairRoot, om.MSpace.kWorld, uvSet=uv_set)
        uvs[i].x = res[0]
        uvs[i].y = res[1]

    schema: abcGeom.OCurvesSchema = curveObj.getSchema()
    cp: abc.OCompoundProperty = schema.getArbGeomParams()
    uv_prop = abc.OV2fArrayProperty(cp, "groom_root_uv")
    uv_prop.setValue(uvs)


# %%
try:
    from PySide6 import QtCore, QtWidgets
    import shiboken6 as shiboken
except:
    from PySide2 import QtCore, QtWidgets
    import shiboken2 as shiboken

import maya.OpenMayaUI as om1ui


def mayaWindow():
    main_window_ptr = om1ui.MQtUtil.mainWindow()
    return shiboken.wrapInstance(int(main_window_ptr), QtWidgets.QWidget)


# %%
class SaveXGenWindow(QtWidgets.QDialog):
    class Content:
        def __init__(self, fnDepNode, showName, Type, groupName, isGuide, bakeUV):
            self.showName = showName
            self.fnDepNode = fnDepNode
            self.Type = Type
            self.groupName = QtWidgets.QLineEdit()
            self.groupName.setText(groupName)
            self.isGuide = QtWidgets.QCheckBox()
            self.isGuide.setChecked(isGuide)
            self.bakeUV = QtWidgets.QCheckBox()
            self.bakeUV.setChecked(bakeUV)

    curveType = "curve"
    xgenType = "xgen"
    instance = None

    def getInstance(self):
        if self.instance is None:
            self.instance = SaveXGenWindow()
        return self.instance

    def __init__(self, parent=mayaWindow()):
        super(SaveXGenWindow, self).__init__(parent)
        self.contentList: List[SaveXGenWindow.Content] = []
        self.save_path = '..'
        self.setWindowTitle("Export XGen to UE Groom")
        self.setGeometry(400, 400, 800, 400)
        self.buildUI()

    def showAbout(self):
        QtWidgets.QMessageBox.about(self, "Export XGen to UE Groom",
                                    "A small tool to export XGen to UE Groom, by PDE26jjk.<br> link:  <a href='https://github.com/PDE26jjk/XGenUEGroomExporter'>https://github.com/PDE26jjk/XGenUEGroomExporter</a>")

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
        self.table.setColumnCount(6)
        self.table.setHorizontalHeaderLabels(["Name", "Type", "Group name", "Is guide", "Bake UV", ""])
        self.table.horizontalHeader().setSectionResizeMode(1, QtWidgets.QHeaderView.ResizeToContents)
        self.table.setColumnWidth(3, 140)
        self.table.horizontalHeader().setSectionResizeMode(3, QtWidgets.QHeaderView.Fixed)
        self.table.setColumnWidth(4, 140)
        self.table.horizontalHeader().setSectionResizeMode(4, QtWidgets.QHeaderView.Fixed)
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

        try:
            self.Bakeframe = om1ui.MQtUtil.findControl(
                cmds.frameLayout(label='Bake UV', collapsable=True, collapse=True, manage=True))
            self.Bakeframe: QtWidgets.QWidget = shiboken.wrapInstance(int(self.Bakeframe), QtWidgets.QWidget)
            self.Bakeframe.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Maximum)
            # self.Bakeframe.setParent(self)
            frameLayout: QtWidgets.QLayout = self.Bakeframe.children()[2].children()[0]
        except:
            self.Bakeframe = QtWidgets.QFrame(self)
            self.Bakeframe.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Maximum)
            frameLayout = QtWidgets.QVBoxLayout(self.Bakeframe)
            self.Bakeframe.children().append(frameLayout)

        self.MeshName = QtWidgets.QLabel(f"Mesh : ---")

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

        self.save_button = QtWidgets.QPushButton("Save Alembic File", self)
        self.save_button.clicked.connect(self.save_abc)

        self.cancel_button = QtWidgets.QPushButton("Close", self)  # 关闭
        self.cancel_button.clicked.connect(self.close)  # 关闭窗口

        button_layout = QtWidgets.QHBoxLayout()
        button_layout.addWidget(self.save_button)
        button_layout.addWidget(self.cancel_button)

        main_layout.addWidget(self.table)
        main_layout.addWidget(self.Bakeframe)
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
        """打开保存文件对话框"""
        if len(self.contentList) == 0:
            print("No content")
            return
        file_path = cmds.fileDialog2(
            dialogStyle=2,
            caption="保存Alembic文件",
            fileMode=0,  # 0: 保存文件
            okCaption="save",
            # defaultExtension='abc',
            startingDirectory=self.save_path,
            ff='Alembic Files (*.abc);;All Files (*)'
        )
        self.save_path = file_path[0]
        startTime = time.time()
        archive = abc.OArchive(file_path[0])
        for item in self.contentList:
            fnDepNode = item.fnDepNode
            curveObj = abcGeom.OCurves(archive.getTop(), fnDepNode.name())
            needBakeUV = item.bakeUV.isChecked()
            if item.Type == SaveXGenWindow.xgenType:
                rootList = write_xgen(curveObj, fnDepNode, needBakeUV)
            elif item.Type == SaveXGenWindow.curveType:
                rootList = write_curves(curveObj, fnDepNode, needBakeUV)
            write_group_and_guide(curveObj, item.groupName.text(), item.isGuide.isChecked())
            if needBakeUV:
                bake_uv(curveObj, rootList, self.bakeMesh, self.uvSetStr.text())
        print(f"Data has been saved in {file_path[0]} , it took {time.time() - startTime:.2f} seconds.")
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
                                           False))
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
                                           False))

        self.table.setRowCount(len(contentList))
        for row in range(len(contentList)):
            item = QtWidgets.QTableWidgetItem(contentList[row].showName)
            item.setFlags(QtCore.Qt.ItemFlag.ItemIsEnabled)  # 不可编辑
            self.table.setItem(row, 0, item)

            item = QtWidgets.QTableWidgetItem(contentList[row].Type)
            # item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
            item.setFlags(QtCore.Qt.ItemFlag.ItemIsEnabled)
            self.table.setItem(row, 1, item)

            self.table.setCellWidget(row, 2, contentList[row].groupName)
            self.table.setCellWidget(row, 3, contentList[row].isGuide)
            self.table.setCellWidget(row, 4, contentList[row].bakeUV)

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


# %%

SaveXGenWindow().getInstance().show()
# %%


# %%
