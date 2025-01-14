# XGenUEGroomExporter
A small tool to export XGen to UE Groom. Select curves and interactive XGen descriptions to export an abc file.

选择曲线和可交互XGen描述，可以导出abc文件。带分组，可以烘焙UV。UE5可识别。

![image-20241108002757544](https://raw.githubusercontent.com/PDE26jjk/misc/main/img/image-20241108002757544.png)

Maya2025版本可用。[用法视频](https://www.bilibili.com/video/BV1U7mzYDEA4)

现在只维护XGenUEGroomExporter.py和XGenDescriptionUEGroomExporter.py及其py2版本。

微信 : XJ845077205
邮箱：26jjk@sina.com

## 更新功能：

- 无需numpy也可以使用
- XGen写入宽度
- 自动生成Group_id，以便低版本UE可以识别分组
- 写入动画
- 增加py2版本，Maya2018可用。
- knots端点处修复，解决UE导入报错
- 新增XGenDescriptionUEGroomExporter.py，专门导出XGen Description。

## XGenUEGroomExporter用法说明：

![image-20250114225246322](https://raw.githubusercontent.com/PDE26jjk/misc/main/img/image-20250114225246322.png)

这个脚本将曲线和可交互XGen导出成UE可识别的Groom。

选择曲线和可交互XGen之后，点击Refresh selected按钮，它们会显示在界面中。将需要作为导向的曲线和对应的可交互XGen设置一样的Group name，曲线勾选Is guide即可。

UV烘焙：将会识别XGen的绑定Mesh，可以按Pick other mesh选择其他Mesh。

选择Animation之后，可以导出动画，需要在Animation选项卡中设置帧范围。如果勾选Preroll，将在记录动画之前从第一帧播放到帧范围开始。

## XGenDescriptionUEGroomExporter用法说明：

![image-20250114223649517](https://raw.githubusercontent.com/PDE26jjk/misc/main/img/image-20250114223649517.png)

这个脚本专门将带有导向的XGen描述导出成UE可识别的Groom。

选择带有导向的XGen描述之后，点击Refresh selected按钮，它们会显示在界面中。点击每个描述，右侧显示其他选项。

如果勾选Animation，将会导出导向动画。

write spline animation 选项：如果勾选，导出动画的时候同时导出Spline的动画，文件可能会很大。

write guide id from ptex 选项：选择一张ptex贴图，将会将对应颜色的导向和发丝对应起来，生成UE可识别的属性。默认从Clumping修改器读取。！！注意：UE5.3以上的版本目前有bug，不会正确读取导向权重属性。

生成过程中产生的交互式XGen会被放到一个父节点中，可以按Clear Temp Data删除它。

# English version
## Usage Instructions for XGenUEGroomExporter:

![image-20250114225246322](https://raw.githubusercontent.com/PDE26jjk/misc/main/img/image-20250114225246322.png)

This script exports curves and interactive XGen descriptions into UE-recognizable Groom format.

After selecting the curves and interactive XGen descriptions, click the **Refresh Selected** button; they will be displayed in the interface. Assign the same **Group Name** to the curves intended as guides and their corresponding interactive XGen descriptions, and check the **Is Guide** option for the curves.

**UV Baking**: It will recognize the bound mesh of the XGen description; you can select another mesh by clicking **Pick Other Mesh**.

Once **Animation** is selected, animated data can be exported, with the frame range to be set in the **Animation** tab. If **Preroll** is checked, it will play from the first frame to the start of the frame range before recording the animation.

## Usage Instructions for XGenDescriptionUEGroomExporter:

![image-20250114223649517](https://raw.githubusercontent.com/PDE26jjk/misc/main/img/image-20250114223649517.png)

This script specifically exports XGen descriptions with guides into UE-recognizable Groom format.

After selecting the XGen descriptions with guides, click the **Refresh Selected** button; they will be displayed in the interface. Click on each description to reveal additional options on the right.

If **Animation** is checked, guide animation will be exported.

**Write Spline Animation** option: If checked, the spline animation will be exported simultaneously with the animation, potentially resulting in a large file size.

**Write Guide ID from Ptex** option: Select a ptex texture; it will correlate the guide corresponding to the color with the hair strand, generating UE-recognizable attributes. By default, it reads from the Clumping modifier. **Note**: There is currently a bug in UE 5.3 and above that prevents the correct reading of guide weight attributes.

The interactive XGen generated during the process will be placed under a parent node, which can be deleted by clicking **Clear Temp Data**.